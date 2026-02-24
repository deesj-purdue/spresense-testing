/****************************************************************************
 * examples/usb_camera/usb_camera_main.c
 *
 * Stream real camera images from the Spresense camera board (ISX012 /
 * ISX019 5-MP sensor) over USB CDC/ACM to a PC host.
 *
 * Two output modes:
 *
 *   JPEG mode (default, recommended):
 *     Captures JPEG-compressed stills from the camera hardware JPEG
 *     encoder and sends them as-is.  Far more bandwidth-efficient than
 *     grayscale for real photographic content.
 *     Header magic = "JPG\0" (0x0047504A).
 *
 *   Grayscale mode (-g flag):
 *     Captures RGB565 video frames, converts to 8-bit grayscale,
 *     RLE-compresses, and sends using the usb_image protocol.
 *     Header magic = "IMG\0" (0x00474D49).
 *     Note: limited to QVGA (320x240) due to Spresense RAM constraints
 *     when using dual RGB565 buffers + grayscale + RLE buffers.
 *
 * Frame protocol (shared with usb_image):
 *   [4 bytes] magic    "IMG\0" for RLE-gray  or  "JPG\0" for JPEG
 *   [2 bytes] width    image width  (little-endian uint16)
 *   [2 bytes] height   image height (little-endian uint16)
 *   [4 bytes] datalen  payload size (little-endian uint32)
 *   [datalen] payload  RLE [count][value] pairs  or  raw JPEG bytes
 *
 * The PC-side usb_image_receiver.py handles both formats.
 *
 * Usage:
 *   usb_camera              # JPEG at VGA   (640x480)
 *   usb_camera -g           # Grayscale RLE at QVGA (320x240)
 *   usb_camera -r qvga      # JPEG at QVGA  (320x240)
 *   usb_camera -r hd        # JPEG at HD    (1280x720)
 *   usb_camera -r quadvga   # JPEG at QuadVGA (1280x960)
 *   usb_camera -r fullhd    # JPEG at FullHD (1920x1080)
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/boardctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <nuttx/usb/usbdev.h>
#include <nuttx/usb/cdcacm.h>
#include <nuttx/video/video.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define USBSER_DEVNAME "/dev/ttyACM0"
#define VIDEO_DEVNAME "/dev/video"

/* USB write chunk size */

#define CHUNK_SIZE 32768

/* Header magic values (little-endian on wire) */

#define HEADER_MAGIC_IMG 0x00474D49u /* 'I','M','G','\0' - RLE gray  */
#define HEADER_MAGIC_JPG 0x0047504Au /* 'J','P','G','\0' - JPEG      */

/* Maximum JPEG frame buffer size.
 * 300 KB is sufficient for up to FullHD JPEG from the ISX012/ISX019.
 */

#define JPEG_BUF_SIZE (300 * 1024)

/* Number of camera buffers */

#define JPEG_BUFNUM 1
#define VIDEO_BUFNUM 2

/* RGB565: 2 bytes per pixel */

#define RGB565_BPP 2

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct __attribute__((packed)) img_header_s
{
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint32_t datalen;
};

/* Resolution look-up entry */

struct res_entry_s
{
    const char *name;
    uint16_t w;
    uint16_t h;
};

/* V4L2 user-pointer buffer descriptor */

struct v_buffer_s
{
    FAR uint32_t *start;
    uint32_t length;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct res_entry_s g_res_table[] =
    {
        {"qvga", VIDEO_HSIZE_QVGA, VIDEO_VSIZE_QVGA},          /* 320x240   */
        {"vga", VIDEO_HSIZE_VGA, VIDEO_VSIZE_VGA},             /* 640x480   */
        {"hd", VIDEO_HSIZE_HD, VIDEO_VSIZE_HD},                /* 1280x720  */
        {"quadvga", VIDEO_HSIZE_QUADVGA, VIDEO_VSIZE_QUADVGA}, /* 1280x960  */
        {"fullhd", VIDEO_HSIZE_FULLHD, VIDEO_VSIZE_FULLHD},    /* 1920x1080 */
        {NULL, 0, 0}};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: write_all
 *
 * Description:
 *   Write exactly 'len' bytes to fd, retrying on partial writes.
 *   Returns 0 on success, -1 on unrecoverable error.
 ****************************************************************************/

static int write_all(int fd, const uint8_t *buf, size_t len)
{
    ssize_t n;

    while (len > 0)
    {
        size_t chunk = len < CHUNK_SIZE ? len : CHUNK_SIZE;

        n = write(fd, buf, chunk);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            printf("usb_camera: write error: %d\n", errno);
            return -1;
        }

        buf += n;
        len -= (size_t)n;
    }

    return 0;
}

/****************************************************************************
 * Name: rle_encode
 *
 * Description:
 *   RLE-encode src into dst.  Format: repeated [count][value] pairs,
 *   count = 1..255.  Identical to the usb_image encoder.
 *   Returns compressed size in bytes.
 ****************************************************************************/

static uint32_t rle_encode(const uint8_t *src, uint32_t src_len,
                           uint8_t *dst, uint32_t dst_max)
{
    uint32_t si = 0;
    uint32_t di = 0;

    while (si < src_len)
    {
        uint8_t val = src[si];
        uint32_t run = 1;

        while (si + run < src_len && src[si + run] == val && run < 255)
        {
            run++;
        }

        if (di + 2 > dst_max)
        {
            break; /* safety guard */
        }

        dst[di++] = (uint8_t)run;
        dst[di++] = val;
        si += run;
    }

    return di;
}

/****************************************************************************
 * Name: rgb565_to_gray
 *
 * Description:
 *   Convert an RGB565 (little-endian) image to 8-bit grayscale using
 *   BT.601 luminance coefficients:  Y = 0.299 R + 0.587 G + 0.114 B
 *   Fixed-point approximation: Y = (R8*77 + G8*150 + B8*29) >> 8
 ****************************************************************************/

static void rgb565_to_gray(const uint8_t *src, uint8_t *dst,
                           uint32_t npixels)
{
    uint32_t i;

    for (i = 0; i < npixels; i++)
    {
        uint16_t px = *(const uint16_t *)src;

        /* Extract and scale to 8-bit */

        uint32_t r = (px >> 8) & 0xf8; /* R5 << 3 */
        uint32_t g = (px >> 3) & 0xfc; /* G6 << 2 */
        uint32_t b = (px << 3) & 0xf8; /* B5 << 3 */

        dst[i] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
        src += 2;
    }
}

/****************************************************************************
 * Name: camera_prepare
 *
 * Description:
 *   Set the V4L2 format, request user-pointer buffers, allocate and
 *   enqueue them, and start the stream.
 ****************************************************************************/

static int camera_prepare(int fd, enum v4l2_buf_type type,
                          uint32_t buf_mode, uint32_t pixfmt,
                          uint16_t w, uint16_t h,
                          FAR struct v_buffer_s **vbuf,
                          uint8_t bufnum, uint32_t bufsize)
{
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    int cnt;
    int ret;

    /* ---- VIDIOC_S_FMT ---- */

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    fmt.fmt.pix.pixelformat = pixfmt;

    ret = ioctl(fd, VIDIOC_S_FMT, (uintptr_t)&fmt);
    if (ret < 0)
    {
        printf("usb_camera: VIDIOC_S_FMT failed: %d\n", errno);
        return -1;
    }

    /* ---- VIDIOC_REQBUFS ---- */

    memset(&req, 0, sizeof(req));
    req.type = type;
    req.memory = V4L2_MEMORY_USERPTR;
    req.count = bufnum;
    req.mode = buf_mode;

    ret = ioctl(fd, VIDIOC_REQBUFS, (uintptr_t)&req);
    if (ret < 0)
    {
        printf("usb_camera: VIDIOC_REQBUFS failed: %d\n", errno);
        return -1;
    }

    /* ---- Allocate buffer descriptors ---- */

    *vbuf = (FAR struct v_buffer_s *)malloc(sizeof(struct v_buffer_s) * bufnum);
    if (!(*vbuf))
    {
        printf("usb_camera: out of memory (v_buffer array)\n");
        return -1;
    }

    for (cnt = 0; cnt < bufnum; cnt++)
    {
        (*vbuf)[cnt].length = bufsize;

        /* V4L2 on Spresense requires 32-byte aligned buffers */

        (*vbuf)[cnt].start = (FAR uint32_t *)memalign(32, bufsize);
        if (!(*vbuf)[cnt].start)
        {
            printf("usb_camera: buffer alloc failed (%d/%d, %lu bytes)\n",
                   cnt, bufnum, (unsigned long)bufsize);
            while (cnt--)
            {
                free((*vbuf)[cnt].start);
            }

            free(*vbuf);
            *vbuf = NULL;
            return -1;
        }
    }

    /* ---- VIDIOC_QBUF — enqueue all buffers ---- */

    for (cnt = 0; cnt < bufnum; cnt++)
    {
        memset(&buf, 0, sizeof(buf));
        buf.type = type;
        buf.memory = V4L2_MEMORY_USERPTR;
        buf.index = cnt;
        buf.m.userptr = (uintptr_t)(*vbuf)[cnt].start;
        buf.length = (*vbuf)[cnt].length;

        ret = ioctl(fd, VIDIOC_QBUF, (uintptr_t)&buf);
        if (ret < 0)
        {
            printf("usb_camera: VIDIOC_QBUF failed: %d\n", errno);

            for (cnt = 0; cnt < bufnum; cnt++)
            {
                free((*vbuf)[cnt].start);
            }

            free(*vbuf);
            *vbuf = NULL;
            return -1;
        }
    }

    /* ---- VIDIOC_STREAMON ---- */

    ret = ioctl(fd, VIDIOC_STREAMON, (uintptr_t)&type);
    if (ret < 0)
    {
        printf("usb_camera: VIDIOC_STREAMON failed: %d\n", errno);

        for (cnt = 0; cnt < bufnum; cnt++)
        {
            free((*vbuf)[cnt].start);
        }

        free(*vbuf);
        *vbuf = NULL;
        return ret;
    }

    return 0;
}

/****************************************************************************
 * Name: free_buffers
 ****************************************************************************/

static void free_buffers(FAR struct v_buffer_s *vbuf, uint8_t bufnum)
{
    uint8_t i;

    if (vbuf)
    {
        for (i = 0; i < bufnum; i++)
        {
            if (vbuf[i].start)
            {
                free(vbuf[i].start);
            }
        }

        free(vbuf);
    }
}

/****************************************************************************
 * Name: get_camimage
 *
 * Description:
 *   DQBUF one camera frame.
 ****************************************************************************/

static int get_camimage(int fd, FAR struct v4l2_buffer *v4l2_buf,
                        enum v4l2_buf_type type)
{
    memset(v4l2_buf, 0, sizeof(*v4l2_buf));
    v4l2_buf->type = type;
    v4l2_buf->memory = V4L2_MEMORY_USERPTR;

    if (ioctl(fd, VIDIOC_DQBUF, (uintptr_t)v4l2_buf) < 0)
    {
        printf("usb_camera: VIDIOC_DQBUF failed: %d\n", errno);
        return -1;
    }

    return 0;
}

/****************************************************************************
 * Name: release_camimage
 *
 * Description:
 *   QBUF — return frame buffer to the camera driver.
 ****************************************************************************/

static int release_camimage(int fd, FAR struct v4l2_buffer *v4l2_buf)
{
    if (ioctl(fd, VIDIOC_QBUF, (uintptr_t)v4l2_buf) < 0)
    {
        printf("usb_camera: VIDIOC_QBUF failed: %d\n", errno);
        return -1;
    }

    return 0;
}

/****************************************************************************
 * Name: lookup_resolution
 ****************************************************************************/

static const struct res_entry_s *lookup_resolution(const char *name)
{
    const struct res_entry_s *r;

    for (r = g_res_table; r->name != NULL; r++)
    {
        if (strcmp(r->name, name) == 0)
        {
            return r;
        }
    }

    return NULL;
}

/****************************************************************************
 * Name: print_usage
 ****************************************************************************/

static void print_usage(void)
{
    printf("Usage: usb_camera [-g] [-r qvga|vga|hd|quadvga|fullhd]\n");
    printf("  -g        Grayscale RLE mode (usb_image compatible)\n");
    printf("  -r RES    Resolution (default: vga for JPEG, qvga for gray)\n");
    printf("  -h        Show this help\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
    struct boardioc_usbdev_ctrl_s ctrl;
    FAR void *handle;
    struct img_header_s hdr;
    FAR struct v_buffer_s *cam_bufs = NULL;
    struct v4l2_buffer v4l2_buf;
    uint8_t *graybuf = NULL;
    uint8_t *rlebuf = NULL;
    int usb_fd = -1;
    int cam_fd = -1;
    int ret;
    uint32_t frame = 0;
    int jpeg_mode = 1;
    const char *res_name = NULL;
    const struct res_entry_s *res;
    enum v4l2_buf_type cap_type;
    uint32_t pixfmt;
    uint32_t bufsize;
    uint8_t bufnum;
    uint16_t img_w;
    uint16_t img_h;
    uint32_t npixels;
    int i;

    /* ================================================================ */
    /*  Parse command-line arguments                                    */
    /* ================================================================ */

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-g") == 0)
        {
            jpeg_mode = 0;
        }
        else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
        {
            res_name = argv[++i];
        }
        else if (strcmp(argv[i], "-h") == 0)
        {
            print_usage();
            return EXIT_SUCCESS;
        }
        else
        {
            printf("usb_camera: unknown option '%s'\n", argv[i]);
            print_usage();
            return EXIT_FAILURE;
        }
    }

    /* Apply defaults: JPEG → VGA, grayscale → QVGA */

    if (res_name == NULL)
    {
        res_name = jpeg_mode ? "vga" : "qvga";
    }

    res = lookup_resolution(res_name);
    if (!res)
    {
        printf("usb_camera: unknown resolution '%s'\n", res_name);
        print_usage();
        return EXIT_FAILURE;
    }

    img_w = res->w;
    img_h = res->h;
    npixels = (uint32_t)img_w * img_h;

    if (jpeg_mode)
    {
        cap_type = V4L2_BUF_TYPE_STILL_CAPTURE;
        pixfmt = V4L2_PIX_FMT_JPEG;
        bufsize = JPEG_BUF_SIZE;
        bufnum = JPEG_BUFNUM;
        printf("usb_camera: JPEG mode  %ux%u  (buf %lu KB)\n",
               img_w, img_h, (unsigned long)(bufsize / 1024));
    }
    else
    {
        cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        pixfmt = V4L2_PIX_FMT_RGB565;
        bufsize = npixels * RGB565_BPP;
        bufnum = VIDEO_BUFNUM;
        printf("usb_camera: Grayscale mode  %ux%u  (buf %lu KB x %d)\n",
               img_w, img_h,
               (unsigned long)(bufsize / 1024), bufnum);
    }

    /* ================================================================ */
    /*  1. Initialise the camera (V4L2)                                 */
    /* ================================================================ */

    printf("usb_camera: initialising camera ...\n");

    ret = video_initialize(VIDEO_DEVNAME);
    if (ret != 0)
    {
        printf("usb_camera: ERROR: video_initialize failed: %d\n", errno);
        return EXIT_FAILURE;
    }

    cam_fd = open(VIDEO_DEVNAME, 0);
    if (cam_fd < 0)
    {
        printf("usb_camera: ERROR: open(%s) failed: %d\n",
               VIDEO_DEVNAME, errno);
        goto cleanup;
    }

    /* Print sensor name */

    {
        struct v4l2_capability cap;

        memset(&cap, 0, sizeof(cap));
        if (ioctl(cam_fd, VIDIOC_QUERYCAP, (uintptr_t)&cap) == 0)
        {
            printf("usb_camera: sensor = %s\n", (const char *)cap.driver);
        }
    }

    /* Prepare camera buffers and start streaming */

    ret = camera_prepare(cam_fd, cap_type,
                         jpeg_mode ? V4L2_BUF_MODE_FIFO : V4L2_BUF_MODE_RING,
                         pixfmt, img_w, img_h,
                         &cam_bufs, bufnum, bufsize);
    if (ret < 0)
    {
        goto cleanup;
    }

    /* For STILL_CAPTURE (JPEG), issue TAKEPICT_START to begin capturing */

    if (jpeg_mode)
    {
        ret = ioctl(cam_fd, VIDIOC_TAKEPICT_START, 0);
        if (ret < 0)
        {
            printf("usb_camera: VIDIOC_TAKEPICT_START failed: %d\n", errno);
            goto cleanup;
        }
    }

    /* Let the sensor auto-exposure / auto-white-balance stabilise */

    printf("usb_camera: waiting for AE/AWB to stabilise ...\n");
    sleep(1);

    /* ================================================================ */
    /*  2. Register USB CDC/ACM device                                  */
    /* ================================================================ */

    printf("usb_camera: registering USB CDC/ACM device ...\n");

    ctrl.usbdev = BOARDIOC_USBDEV_CDCACM;
    ctrl.action = BOARDIOC_USBDEV_CONNECT;
    ctrl.instance = 0;
    ctrl.handle = &handle;

    ret = boardctl(BOARDIOC_USBDEV_CONTROL, (uintptr_t)&ctrl);
    if (ret < 0)
    {
        printf("usb_camera: WARNING: USB serial may already be "
               "registered (%d)\n",
               -ret);
    }
    else
    {
        printf("usb_camera: USB serial driver registered OK\n");
    }

    /* ================================================================ */
    /*  3. Open USB serial port (blocks until host connects)            */
    /* ================================================================ */

    printf("usb_camera: waiting for USB host on %s ...\n", USBSER_DEVNAME);

    do
    {
        usb_fd = open(USBSER_DEVNAME, O_WRONLY);
        if (usb_fd < 0)
        {
            if (errno == ENOTCONN)
            {
                sleep(1);
                continue;
            }

            printf("usb_camera: ERROR: open(%s) failed: %d\n",
                   USBSER_DEVNAME, errno);
            goto cleanup;
        }
    } while (usb_fd < 0);

    printf("usb_camera: USB host connected!\n");

    /* ================================================================ */
    /*  4. Allocate conversion buffers (grayscale mode only)            */
    /* ================================================================ */

    if (!jpeg_mode)
    {
        graybuf = (uint8_t *)malloc(npixels);
        rlebuf = (uint8_t *)malloc(npixels * 2);

        if (!graybuf || !rlebuf)
        {
            printf("usb_camera: ERROR: failed to allocate gray/rle "
                   "buffers (%lu bytes)\n",
                   (unsigned long)(npixels + npixels * 2));
            goto cleanup;
        }
    }

    /* ================================================================ */
    /*  5. Prepare frame header template                                */
    /* ================================================================ */

    hdr.magic = jpeg_mode ? HEADER_MAGIC_JPG : HEADER_MAGIC_IMG;
    hdr.width = img_w;
    hdr.height = img_h;
    /* hdr.datalen is set per frame */

    /* ================================================================ */
    /*  6. Main streaming loop                                          */
    /* ================================================================ */

    printf("usb_camera: streaming %ux%u %s frames over USB.\n",
           img_w, img_h, jpeg_mode ? "JPEG" : "grayscale");

    for (frame = 0; /* forever */; frame++)
    {
        /* ---- Dequeue a filled camera buffer ---- */

        if (get_camimage(cam_fd, &v4l2_buf, cap_type) < 0)
        {
            break;
        }

        if (jpeg_mode)
        {
            /* ---- JPEG: send raw JPEG bytes ---- */

            hdr.datalen = v4l2_buf.bytesused;

            if (write_all(usb_fd,
                          (const uint8_t *)&hdr, sizeof(hdr)) < 0)
            {
                release_camimage(cam_fd, &v4l2_buf);
                break;
            }

            if (write_all(usb_fd,
                          (const uint8_t *)v4l2_buf.m.userptr,
                          v4l2_buf.bytesused) < 0)
            {
                release_camimage(cam_fd, &v4l2_buf);
                break;
            }
        }
        else
        {
            /* ---- Grayscale: RGB565 → gray → RLE ---- */

            uint32_t rle_size;

            rgb565_to_gray((const uint8_t *)v4l2_buf.m.userptr,
                           graybuf, npixels);

            /* Return the camera buffer ASAP to free memory pressure */

            release_camimage(cam_fd, &v4l2_buf);

            rle_size = rle_encode(graybuf, npixels,
                                  rlebuf, npixels * 2);
            hdr.datalen = rle_size;

            if (write_all(usb_fd,
                          (const uint8_t *)&hdr, sizeof(hdr)) < 0)
            {
                break;
            }

            if (write_all(usb_fd, rlebuf, rle_size) < 0)
            {
                break;
            }

            /* Skip the release below — already done above */

            goto print_status;
        }

        /* Return the camera buffer */

        release_camimage(cam_fd, &v4l2_buf);

    print_status:
        /* Print a status line every 32 frames */

        if ((frame & 0x1f) == 0)
        {
            printf("usb_camera: frame %lu  %s  payload=%lu bytes\n",
                   (unsigned long)frame,
                   jpeg_mode ? "jpeg" : "rle",
                   (unsigned long)hdr.datalen);
        }
    }

    printf("usb_camera: streaming stopped at frame %lu.\n",
           (unsigned long)frame);

    /* ================================================================ */
    /*  7. Cleanup                                                      */
    /* ================================================================ */

cleanup:

    if (jpeg_mode && cam_fd >= 0)
    {
        ioctl(cam_fd, VIDIOC_TAKEPICT_STOP, 0);
    }

    if (cam_fd >= 0)
    {
        close(cam_fd);
    }

    free_buffers(cam_bufs, bufnum);
    free(graybuf);
    free(rlebuf);

    if (usb_fd >= 0)
    {
        close(usb_fd);
    }

    ctrl.action = BOARDIOC_USBDEV_DISCONNECT;
    boardctl(BOARDIOC_USBDEV_CONTROL, (uintptr_t)&ctrl);

    video_uninitialize(VIDEO_DEVNAME);

    printf("usb_camera: done.\n");
    return EXIT_SUCCESS;
}
