#!/usr/bin/env python3
"""Send an image or animated GIF to USB2TFT over its USB CDC port."""

import argparse
import struct
import sys
import time

from PIL import Image, ImageOps, ImageSequence
import serial

WIDTH = 128
HEIGHT = 128
FRAME_BYTES = WIDTH * HEIGHT * 2
HEADER = b"TFT1" + struct.pack(">I", FRAME_BYTES)


def rgb565_be(image: Image.Image) -> bytes:
    """Centre-crop *image* to the TFT aspect ratio and return RGB565 bytes."""
    image = ImageOps.fit(image.convert("RGB"), (WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    output = bytearray(FRAME_BYTES)
    index = 0
    for red, green, blue in image.getdata():
        pixel = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        output[index] = pixel >> 8
        output[index + 1] = pixel & 0xFF
        index += 2
    return bytes(output)


def load_frames(path: str):
    with Image.open(path) as image:
        frames = []
        for frame in ImageSequence.Iterator(image):
            duration_ms = frame.info.get("duration", image.info.get("duration", 100))
            frames.append((rgb565_be(frame), max(duration_ms, 10) / 1000))
    return frames


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB CDC port, e.g. /dev/ttyACM0 or COM3")
    parser.add_argument("image", help="Image or animated GIF to display")
    parser.add_argument("--baud", type=int, default=115200,
                        help="CDC port setting (not a USB speed limit; default: 115200)")
    parser.add_argument("--fps", type=float, help="Override the source frame rate")
    parser.add_argument("--loop", action="store_true", help="Repeat the animation or image forever")
    args = parser.parse_args()

    if args.fps is not None and args.fps <= 0:
        parser.error("--fps must be greater than zero")

    frames = load_frames(args.image)
    if not frames:
        print("No frames found in input image.", file=sys.stderr)
        return 1

    with serial.Serial(args.port, args.baud, timeout=2, write_timeout=5) as device:
        # Opening a CDC port can toggle DTR. Let the RP2040 finish USB setup.
        time.sleep(0.25)
        while True:
            for pixels, source_delay in frames:
                start = time.monotonic()
                device.write(HEADER)
                device.write(pixels)
                device.flush()
                delay = 1 / args.fps if args.fps else source_delay
                remaining = delay - (time.monotonic() - start)
                if remaining > 0:
                    time.sleep(remaining)
            if not args.loop:
                break
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
