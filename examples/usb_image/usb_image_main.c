/****************************************************************************
 * examples/usb_image/usb_image_main.c
 *
 * Send 640x480 8-bit grayscale dummy images over USB CDC/ACM to a PC.
 *
 * Protocol (per frame):
 *   [4 bytes] magic   = 0x494D4700  ("IMG\0")
 *   [2 bytes] width   = 640   (little-endian uint16)
 *   [2 bytes] height  = 480   (little-endian uint16)
 *   [4 bytes] datalen = 307200 (little-endian uint32) = width * height
 *   [datalen bytes] raw pixel data (row-major, 1 byte per pixel)
 *
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

#define USBSER_DEVNAME "/dev/ttyACM0"

/* How many bytes to write() at once.
 * Larger chunks = fewer syscalls = higher throughput. */
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
    uint32_t datalen;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * Generate a dummy gradient test pattern.
 * Frame counter is used to animate the pattern so each frame is different.
 */
static void generate_dummy_image(uint8_t *buf, uint32_t frame)
{
    int x;
    int y;
    uint8_t offset = (uint8_t)(frame & 0xff);

    for (y = 0; y < IMG_HEIGHT; y++)
    {
        for (x = 0; x < IMG_WIDTH; x++)
        {
            /* Diagonal gradient + animation offset */
            buf[y * IMG_WIDTH + x] = (uint8_t)((x + y + offset) & 0xff);
        }
    }
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
    uint32_t frame;
    int fd;
    int ret;

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

    /* --- 3. Allocate image buffer --- */

    imgbuf = (uint8_t *)malloc(IMG_SIZE);
    if (!imgbuf)
    {
        printf("usb_image: ERROR: failed to allocate %d bytes\n", IMG_SIZE);
        close(fd);
        return EXIT_FAILURE;
    }

    /* --- 4. Prepare the fixed header --- */

    hdr.magic = HEADER_MAGIC;
    hdr.width = IMG_WIDTH;
    hdr.height = IMG_HEIGHT;
    hdr.datalen = IMG_SIZE;

    /* --- 5. Send frames in a loop --- */

    printf("usb_image: sending %dx%d frames. Press Ctrl-C to stop.\n",
           IMG_WIDTH, IMG_HEIGHT);

    for (frame = 0;; frame++)
    {
        /* Generate new dummy image */

        generate_dummy_image(imgbuf, frame);

        /* Send header */

        if (write_all(fd, (const uint8_t *)&hdr, sizeof(hdr)) < 0)
        {
            printf("usb_image: lost connection at frame %lu\n",
                   (unsigned long)frame);
            break;
        }

        /* Send pixel data */

        if (write_all(fd, imgbuf, IMG_SIZE) < 0)
        {
            printf("usb_image: lost connection at frame %lu\n",
                   (unsigned long)frame);
            break;
        }

        if ((frame & 0x1f) == 0)
        {
            printf("usb_image: sent frame %lu\n", (unsigned long)frame);
        }
    }

    /* --- 6. Cleanup --- */

    free(imgbuf);
    close(fd);

    ctrl.action = BOARDIOC_USBDEV_DISCONNECT;
    boardctl(BOARDIOC_USBDEV_CONTROL, (uintptr_t)&ctrl);

    printf("usb_image: done.\n");

    return EXIT_SUCCESS;
}
