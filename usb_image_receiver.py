"""
usb_image_receiver.py — PC-side script to receive images from the Spresense
usb_image (grayscale RLE) or usb_camera (JPEG / grayscale RLE) apps over
USB CDC/ACM.

Supported frame formats (auto-detected per frame):
  - "IMG\\0" (0x00474D49): 8-bit grayscale, RLE-compressed  (usb_image / usb_camera -g)
  - "JPG\\0" (0x0047504A): JPEG from the camera hardware    (usb_camera, default)

Requirements:
    pip install pyserial opencv-python numpy

Usage:
    python usb_image_receiver.py          # auto-detect COM port
    python usb_image_receiver.py COM6     # explicit COM port

Press 'q' in the OpenCV window to quit.
"""

import sys
import struct
import time
import serial
import serial.tools.list_ports
import numpy as np

# ---- Try to import OpenCV; fall back to file-saving mode ----
try:
    import cv2

    HAS_CV2 = True
except ImportError:
    HAS_CV2 = False
    print("[WARN] opencv-python not installed — frames will be saved to disk")

# ---- Protocol constants (must match usb_image / usb_camera) ----
HEADER_MAGIC_IMG = 0x00474D49  # 'I','M','G','\0'  RLE grayscale
HEADER_MAGIC_JPG = 0x0047504A  # 'J','P','G','\0'  JPEG
VALID_MAGICS = {HEADER_MAGIC_IMG, HEADER_MAGIC_JPG}

HEADER_SIZE = 12  # 4 + 2 + 2 + 4 bytes
HEADER_STRUCT = "<IHHl"  # magic(u32) width(u16) height(u16) datalen(u32)
BAUD = 115200 * 8


def find_spresense_port():
    """Return the first COM port whose VID:PID matches the Spresense CDC/ACM
    (Sony 054c:0bc2), or None."""
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x054C and p.pid == 0x0BC2:
            return p.device
    return None


def read_exact(ser, n):
    """Read exactly n bytes from the serial port."""
    parts = []
    remaining = n
    while remaining > 0:
        chunk = ser.read(min(remaining, 65536))
        if not chunk:
            continue  # timeout, keep trying
        parts.append(chunk)
        remaining -= len(chunk)
    return b"".join(parts)


def resync(ser):
    """Discard bytes until we find a valid 4-byte magic sequence.
    Returns the magic value that was found."""
    window = b""
    magic_img = struct.pack("<I", HEADER_MAGIC_IMG)
    magic_jpg = struct.pack("<I", HEADER_MAGIC_JPG)
    while True:
        b = ser.read(1)
        if not b:
            continue
        window = (window + b)[-4:]
        if window == magic_img:
            return HEADER_MAGIC_IMG
        if window == magic_jpg:
            return HEADER_MAGIC_JPG


def main():
    # ---- Determine COM port ----
    if len(sys.argv) > 1:
        port = sys.argv[1]
    else:
        port = find_spresense_port()
        if port is None:
            print("ERROR: Could not auto-detect Spresense CDC/ACM port.")
            print("       Specify it manually:  python usb_image_receiver.py COM7")
            sys.exit(1)

    print(f"Opening {port} at {BAUD} baud ...")
    ser = serial.Serial(port, BAUD, timeout=1)
    time.sleep(0.5)  # let the port settle
    ser.reset_input_buffer()

    print("Waiting for image frames (IMG or JPG magic) ...")
    frame_num = 0
    fps_start = time.time()
    fps_count = 0
    fps = 0.0
    window_name = "Spresense USB Camera"

    while True:
        # ---- Read & validate header ----
        raw_hdr = read_exact(ser, HEADER_SIZE)
        magic, width, height, datalen = struct.unpack(HEADER_STRUCT, raw_hdr)

        if magic not in VALID_MAGICS:
            print(f"[WARN] Bad magic 0x{magic:08X}, resyncing ...")
            magic = resync(ser)
            # After resync we already consumed the magic; read rest of header
            rest = read_exact(ser, HEADER_SIZE - 4)
            width, height, datalen = struct.unpack("<HHl", rest)

        if width == 0 or height == 0 or datalen <= 0:
            print(
                f"[WARN] Invalid header w={width} h={height} len={datalen}, resyncing ..."
            )
            resync(ser)
            continue

        # ---- Read payload ----
        payload = read_exact(ser, datalen)

        # ---- Decode based on magic ----
        if magic == HEADER_MAGIC_JPG:
            # JPEG frame from usb_camera
            fmt_tag = "jpeg"
            raw_size = datalen  # for ratio display, compare to uncompressed
            uncomp_size = width * height * 3  # approximate RGB uncompressed

            if HAS_CV2:
                jpg_arr = np.frombuffer(payload, dtype=np.uint8)
                img = cv2.imdecode(jpg_arr, cv2.IMREAD_COLOR)
                if img is None:
                    print(f"[WARN] Failed to decode JPEG ({datalen} bytes), skipping")
                    continue
            else:
                img = None  # will save raw JPEG to disk

        else:
            # RLE grayscale frame from usb_image / usb_camera -g
            fmt_tag = "rle"
            raw_size = width * height
            uncomp_size = raw_size

            img_flat = np.zeros(raw_size, dtype=np.uint8)
            si = 0
            di = 0
            while si + 1 < len(payload) and di < raw_size:
                count = payload[si]
                value = payload[si + 1]
                end = min(di + count, raw_size)
                img_flat[di:end] = value
                di = end
                si += 2

            img = img_flat.reshape((height, width))

        # ---- Statistics ----
        frame_num += 1
        fps_count += 1
        now = time.time()
        elapsed = now - fps_start
        if elapsed >= 1.0:
            fps = fps_count / elapsed
            fps_count = 0
            fps_start = now
        ratio = 100.0 * datalen / uncomp_size if uncomp_size > 0 else 0
        print(
            f"Frame {frame_num}: {width}x{height}  {fmt_tag}={datalen}  "
            f"ratio={ratio:.1f}%  {fps:.1f} FPS"
        )

        # ---- Display / save ----
        if HAS_CV2 and img is not None:
            cv2.imshow(window_name, img)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break
        elif img is not None:
            # Save grayscale as PGM
            fname = f"frame_{frame_num:05d}.pgm"
            with open(fname, "wb") as f:
                f.write(f"P5\n{width} {height}\n255\n".encode())
                f.write(img.tobytes())
            print(f"  -> saved {fname}")
        else:
            # Save raw JPEG (no OpenCV)
            fname = f"frame_{frame_num:05d}.jpg"
            with open(fname, "wb") as f:
                f.write(payload)
            print(f"  -> saved {fname}")

    ser.close()
    if HAS_CV2:
        cv2.destroyAllWindows()
    print("Done.")


if __name__ == "__main__":
    main()

