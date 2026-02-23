/****************************************************************************
 * examples/usb_image/usb_image_main.c
 *
 * Send 640x480 8-bit grayscale dummy images over USB CDC/ACM to a PC.
 *
 * Protocol (per frame):
 *   [4 bytes] magic   = 0x494D4700  ("IMG\0")
 *   [2 bytes] width   = 640   (little-endian uint16)
 *   [2 bytes] height  = 480   (little-endian uint16)
 *   [4 bytes] datalen = compressed size (little-endian uint32)
 *   [datalen bytes] RLE-compressed pixel data
 *
 * RLE encoding: pairs of [count][value], count = 1..255.
 * Header is 12 bytes total.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/boardctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <nuttx/usb/usbdev.h>
#include <nuttx/usb/cdcacm.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define IMG_WIDTH 640
#define IMG_HEIGHT 480
#define IMG_SIZE (IMG_WIDTH * IMG_HEIGHT) /* 307200 bytes */

/* Worst case RLE: no runs, every pixel different = 2 * IMG_SIZE */
#define RLE_MAX_SIZE (IMG_SIZE * 2)

#define USBSER_DEVNAME "/dev/ttyACM0"

/* How many bytes to write() at once. */
#define CHUNK_SIZE 32768

/* Magic bytes: 'I' 'M' 'G' '\0' */
#define HEADER_MAGIC 0x00474D49u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct __attribute__((packed)) img_header_s
{
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint32_t datalen; /* compressed size in bytes */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint32_t g_rng_state = 123456789u;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * Fast xorshift32 PRNG — no stdlib dependency.
 */
static uint32_t xorshift32(void)
{
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x;
    return x;
}

/**
 * Generate a black image with random white pixels.
 * density_pct: 0 = all black, 100 = all white.
 * threshold = (density_pct / 100) * UINT32_MAX.
 */
static void generate_dummy_image(uint8_t *buf, uint32_t frame,
                                 uint32_t threshold)
{
    uint32_t i;
    (void)frame;

    for (i = 0; i < IMG_SIZE; i++)
    {
        buf[i] = (xorshift32() < threshold) ? 255 : 0;
    }
}

/**
 * RLE-encode src (raw pixels) into dst.
 * Format: repeated [count][value] pairs, count = 1..255.
 * Returns the compressed size in bytes.
 */
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
            /* Should never happen with proper dst_max, but guard anyway */
            break;
        }

        dst[di++] = (uint8_t)run;
        dst[di++] = val;
        si += run;
    }

    return di;
}

/**
 * Write exactly 'len' bytes, retrying on partial writes.
 * Returns 0 on success, -1 on error.
 */
static int write_all(int fd, const uint8_t *buf, size_t len)
{
    ssize_t nwritten;

    while (len > 0)
    {
        size_t chunk = len < CHUNK_SIZE ? len : CHUNK_SIZE;
        nwritten = write(fd, buf, chunk);
        if (nwritten < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            printf("usb_image: write error: %d\n", errno);
            return -1;
        }

        buf += nwritten;
        len -= nwritten;
    }

    return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
    struct boardioc_usbdev_ctrl_s ctrl;
    FAR void *handle;
    struct img_header_s hdr;
    uint8_t *imgbuf;
    uint8_t *rlebuf;
    uint32_t rle_size;
    uint32_t frame;
    int density_pct = 50; /* default 50% white pixel density */
    uint32_t threshold;
    int fd;
    int ret;

    /* Parse optional density argument: usb_image [density%] */

    if (argc > 1)
    {
        density_pct = atoi(argv[1]);
        if (density_pct < 0)
            density_pct = 0;
        if (density_pct > 100)
            density_pct = 100;
    }

    /* Convert percentage to uint32 threshold for xorshift comparison.
     * threshold = density_pct * (UINT32_MAX / 100)
     */

    threshold = (uint32_t)((uint64_t)density_pct * 0xFFFFFFFFu / 100u);

    printf("usb_image: white pixel density = %d%%  (threshold = %lu)\n",
           density_pct, (unsigned long)threshold);

    /* --- 1. Register CDC/ACM USB device --- */

    printf("usb_image: registering USB CDC/ACM device...\n");

    ctrl.usbdev = BOARDIOC_USBDEV_CDCACM;
    ctrl.action = BOARDIOC_USBDEV_CONNECT;
    ctrl.instance = 0;
    ctrl.handle = &handle;

    ret = boardctl(BOARDIOC_USBDEV_CONTROL, (uintptr_t)&ctrl);
    if (ret < 0)
    {
        printf("usb_image: WARNING: USB serial may already be registered (%d)\n",
               -ret);
    }
    else
    {
        printf("usb_image: USB serial driver registered OK\n");
    }

    /* --- 2. Open the USB serial port (blocks until host connects) --- */

    printf("usb_image: waiting for USB host to connect on %s ...\n",
           USBSER_DEVNAME);

    do
    {
        fd = open(USBSER_DEVNAME, O_WRONLY);
        if (fd < 0)
        {
            if (errno == ENOTCONN)
            {
                sleep(1);
                continue;
            }
            printf("usb_image: ERROR opening %s: %d\n",
                   USBSER_DEVNAME, errno);
            return EXIT_FAILURE;
        }
    } while (fd < 0);

    printf("usb_image: USB host connected!\n");

    /* --- 3. Allocate buffers --- */

    imgbuf = (uint8_t *)malloc(IMG_SIZE);
    if (!imgbuf)
    {
        printf("usb_image: ERROR: failed to allocate image buffer\n");
        close(fd);
        return EXIT_FAILURE;
    }

    rlebuf = (uint8_t *)malloc(RLE_MAX_SIZE);
    if (!rlebuf)
    {
        printf("usb_image: ERROR: failed to allocate RLE buffer\n");
        free(imgbuf);
        close(fd);
        return EXIT_FAILURE;
    }

    /* --- 4. Prepare the header template --- */

    hdr.magic = HEADER_MAGIC;
    hdr.width = IMG_WIDTH;
    hdr.height = IMG_HEIGHT;
    /* hdr.datalen set per frame */

    /* --- 5. Send frames in a loop --- */

    printf("usb_image: sending %dx%d RLE-compressed frames.\n",
           IMG_WIDTH, IMG_HEIGHT);

    for (frame = 0;; frame++)
    {
        /* Generate new dummy image */

        generate_dummy_image(imgbuf, frame, threshold);

        /* RLE compress */

        rle_size = rle_encode(imgbuf, IMG_SIZE, rlebuf, RLE_MAX_SIZE);
        hdr.datalen = rle_size;

        /* Send header */

        if (write_all(fd, (const uint8_t *)&hdr, sizeof(hdr)) < 0)
        {
            printf("usb_image: lost connection at frame %lu\n",
                   (unsigned long)frame);
            break;
        }

        /* Send compressed data */

        if (write_all(fd, rlebuf, rle_size) < 0)
        {
            printf("usb_image: lost connection at frame %lu\n",
                   (unsigned long)frame);
            break;
        }

        if ((frame & 0x1f) == 0)
        {
            printf("usb_image: frame %lu  raw=%d  rle=%lu  ratio=%.1f%%\n",
                   (unsigned long)frame,
                   IMG_SIZE,
                   (unsigned long)rle_size,
                   100.0f * rle_size / IMG_SIZE);
        }
    }

    /* --- 6. Cleanup --- */

    free(rlebuf);
    free(imgbuf);
    close(fd);

    ctrl.action = BOARDIOC_USBDEV_DISCONNECT;
    boardctl(BOARDIOC_USBDEV_CONTROL, (uintptr_t)&ctrl);

    printf("usb_image: done.\n");

    return EXIT_SUCCESS;
}
