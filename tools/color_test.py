#!/usr/bin/env python3
"""Paint known RGB565 words on USB2TFT so colour order can be read off the panel.

This panel's colour filter is wired B-G-R, which is corrected by the MADCTL BGR
bit in the firmware. If that bit is wrong, the red and blue bars trade places;
green never moves, so green staying put while red and blue swap is the signature
of a colour-order fault rather than a bad frame.
"""

import argparse
import struct
import sys
import time

import serial

WIDTH = 128
HEIGHT = 128
FRAME_BYTES = WIDTH * HEIGHT * 2
HEADER = b"TFT1" + struct.pack(">I", FRAME_BYTES)

PATTERNS = {
    # name: list of (label, RGB565 word) painted as equal vertical bars
    "bars": [("RED", 0xF800), ("GREEN", 0x07E0), ("BLUE", 0x001F)],
    "greys": [("BLACK", 0x0000), ("MID GREY", 0x8410), ("WHITE", 0xFFFF)],
    "mix": [("YELLOW", 0xFFE0), ("CYAN", 0x07FF), ("MAGENTA", 0xF81F)],
}


def vertical_bars(words) -> bytes:
    """Build one frame of equal-width vertical bars, big-endian RGB565."""
    count = len(words)
    row = bytearray()
    for x in range(WIDTH):
        word = words[min(x * count // WIDTH, count - 1)]
        row += struct.pack(">H", word)
    return bytes(row) * HEIGHT


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB CDC port, e.g. /dev/ttyACM0")
    parser.add_argument("--pattern", choices=sorted(PATTERNS), default="bars")
    parser.add_argument("--all", action="store_true",
                        help="Cycle every pattern, holding each one")
    parser.add_argument("--hold", type=float, default=4.0,
                        help="Seconds to hold each pattern (default: 4)")
    args = parser.parse_args()

    names = sorted(PATTERNS) if args.all else [args.pattern]

    with serial.Serial(args.port, 115200, timeout=1, write_timeout=5) as device:
        time.sleep(0.25)
        for name in names:
            entries = PATTERNS[name]
            labels = " | ".join(label for label, _ in entries)
            words = " ".join(f"0x{word:04X}" for _, word in entries)
            print(f"{name}: sending {words}")
            print(f"  left to right the panel should read:  {labels}")

            device.write(HEADER)
            device.write(vertical_bars([word for _, word in entries]))
            device.flush()
            if len(names) > 1 or args.hold:
                time.sleep(args.hold)

    print("\nIf RED and BLUE are swapped while GREEN stays put, the MADCTL BGR "
          "bit in st7735_init() is wrong.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
