## Description

- Goal: Improve the image transmission speed between the Spresence board and computer to 30FPS at VGA resolution.

# Sony SDK

https://github.com/sonydevworld/spresense

My experience has been less than ideal from the SDK. It feels clunky and is slow to compile, often taking 10s of minutes on my laptop. However, it is very feature rich, so this overhead comes with the benefit of many pre-installed features. It is very important to familiarize yourself with the NuttX app-style compilation, which is very different than somethings like the ESP or STM.

# Method

The Sony Spresense doesn’t expose high-speed USB on either the main or the extension board, but there are a few work arounds.

1. USB CDC: Serial communication over the USB port, slow but significantly faster than the Arduino method
2. USB MSC: Computer recognizes it as a storage device, not as direct or fast though
3. Ethernet: Ethernet over USB is possible but less supported, an external adapter my allow it to work fully and reach significantly higher speeds than serial

I choose the USB CDC method because it is well integrated into the Sony Spresence and would require little change to the current algorithm. The other avenues are worth exploring if we cannot achieve a fast enough connection, but they might require additional expansion boards.

# Custom App

To flash custom code on the Sony Spresense you must make a NuttX app. There are several places you can, but I did it in `./examples` folder, with 2 apps, `/usb_image` and `/usb_camera`, for dummy image generation for testing and use with the onboard camera.

To properly format a custom app, the required `Kconfig`, `Make.defs`, and  `Makefile` must be populated. I just used very default settings, nothing special and these can be inferred and copied from other examples.

# Algorithm

Data compression is vital for high speed image transfer, because the CPU is much faster than the transfer speed, so we need to use more CPU to make a smaller image to have faster USB. I implemented both JPG and RLE (run length encoding) for testing. Since the depth images we will be dealing with have a confidence threshold, there are large sections of values chopped for lack of confidence. This enables RLE to be extremely effective at image compression, so I did testing on images with about the same percentage of off pixels as the real depth images will be.

# Basics of USB CDC

### 0. Important includes

Among many other includes, these are the primary drivers for the USB CDC and camera

```c
// Device control and registering operations
#include <sys/ioctl.h>
#include <sys/boardctl.h>

// USB CDC drivers
#include <nuttx/usb/usbdev.h>
#include <nuttx/usb/cdcacm.h>

// Camera drivers
#include <nuttx/video/video.h>
```

### 1. **USB CDC/ACM Device Registration**

- The USB serial communication is implemented using the USB CDC/ACM (Communications Device Class / Abstract Control Model) protocol.
- The `boardctl()` function is used to register the USB CDC/ACM device:
    
    ```c
    struct boardioc_usbdev_ctrl_s ctrl;
    ctrl.usbdev = BOARDIOC_USBDEV_CDCACM;
    ctrl.action = BOARDIOC_USBDEV_CONNECT;
    ctrl.instance = 0;
    ctrl.handle = &handle;
    boardctl(BOARDIOC_USBDEV_CONTROL, (uintptr_t)&ctrl);
    ```
    
- This initializes the USB serial driver and connects it to the host.

### 2. **USB Serial Port Handling**

- The USB serial device is represented as `/dev/ttyACM0` (defined as `USBSER_DEVNAME`).
- The program waits for the USB host to connect by repeatedly attempting to open the device:
    
    ```c
    do {
        usb_fd = open(USBSER_DEVNAME, O_WRONLY);
        if (usb_fd < 0 && errno == ENOTCONN) {
            sleep(1);
        }
    } while (usb_fd < 0);
    ```
    
- Once the host connects, the USB serial port is ready for data transmission.

### 3. **High-Speed Data Transmission**

- The file uses a chunked write mechanism.
- The `write_all()` function handle partial writes and retries:
    
    ```c
    static int write_all(int fd, const uint8_t *buf, size_t len) {
        while (len > 0) {
            size_t chunk = len < CHUNK_SIZE ? len : CHUNK_SIZE;
            ssize_t n = write(fd, buf, chunk);
            if (n < 0 && errno != EINTR) {
                return -1;
            }
            buf += n;
            len -= n;
        }
        return 0;
    }
    ```
    
- The `CHUNK_SIZE` is set to 32768 B to optimize the write performance, but this can be adjusted.

### 4. **Frame Protocol**

- Each frame of data sent over USB includes a custom header followed by the image payload:
    - **Header**:
        - Magic value (`"JPG\\0"` for JPEG or `"IMG\\0"` for grayscale).
        - Image width and height.
        - Payload size.
    - **Payload**:
        - Raw JPEG bytes (for JPEG mode).
        - RLE-compressed grayscale data (for grayscale mode).
- The header and payload are written sequentially to the USB serial port:
    
    ```c
    write_all(usb_fd, (const uint8_t *)&hdr, sizeof(hdr));
    write_all(usb_fd, (const uint8_t *)v4l2_buf.m.userptr, v4l2_buf.bytesused);
    ```
    

### 5. **Error Handling and Cleanup**

- The program handles errors during USB communication (e.g., write failures) and ensures proper cleanup:
    - Buffers are freed.
    - The USB device is disconnected using `boardctl()`:
        
        ```c
        ctrl.action = BOARDIOC_USBDEV_DISCONNECT;
        boardctl(BOARDIOC_USBDEV_CONTROL, (uintptr_t)&ctrl);
        ```
        

# Performance

Raw camera: Using the camera and JPG compression, a VGA image was sent at 4-5 fps, not ideal but absolutely usable for real time. This is significantly faster than the Arduino implementation, but still not perfect. However, we will not be transferring raw images.

Simulated depth image: In the other app, I generate a random image with lots of empty space just like the confidence threshold will have. Using RLE on this image, there was about 15-20 fps. This is much better and much more real time, it doesn’t hit the 30 fps desired but with extra optimizations on top of this prototype it is definitely possible.

---

---

---

# TL;DR Summary

- USB CDC implemented on the Sony Spresence SDK.
- 4-5 fps VGA transmitting raw camera data JPG compressed
- 15-20 fps VGA transmitting simulated depth image (largely blank data) RLE compressed
- More options for further speed increases including switching to ethernet over USB with adaptors, better compression optimized for what the use case will be, adjustments to USB protocol when optimizing for final environment

---

---

---

# Repo and Modified Files:

```jsx
git diff --name-status e9a4f1702e426cb7c85b9e5ea8b062a7ee8978ea 58d9477ee42f548c747c694a846bec963117cbd3
A       NOTES_AND_COMMANDS.md
A       examples/usb_camera/Kconfig
A       examples/usb_camera/Make.defs
A       examples/usb_camera/Makefile
A       examples/usb_camera/usb_camera_main.c
A       examples/usb_image/Kconfig
A       examples/usb_image/Make.defs
A       examples/usb_image/Makefile
A       examples/usb_image/usb_image_main.c
A       flash_writer.exe
A       sdk/configs/examples/usb_camera/defconfig
A       sdk/configs/examples/usb_image/defconfig
A       sdk/flash_writer.exe
A       sdk/serialshell.py
A       serialshell.py
A       usb_image_receiver.py
```

https://github.com/deesj-purdue/spresense-testing 

`$USER$\Documents\.Purdue_test\spresense`