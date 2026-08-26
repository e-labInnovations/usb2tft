#!/usr/bin/env python3
"""Paint a one-pixel border so the panel's GRAM offset can be read off the glass.

The ST7735S has 132 x 162 of GRAM and this panel's glass shows a 128 x 128
window inside it, so CASET/RASET need TFT_COL_OFFSET / TFT_ROW_OFFSET in the
firmware.  Get those wrong and the frame lands off-centre: the missing side
tells you which offset is wrong and by how much.

Each edge is a different colour, one pixel wide, on black:

    top    RED      bottom  GREEN
    left   BLUE     right   WHITE

All four lines fully visible, corners meeting, means the offsets are right.
A missing line means the frame is shifted away from that edge; the white strip
of uninitialised GRAM that used to sit on the right and bottom is the symptom
this test was written for.  The 8-pixel yellow tick at the top left fixes the
orientation so a mirrored MADCTL cannot be mistaken for a bad offset.
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

BLACK = 0x0000
RED = 0xF800
GREEN = 0x07E0
BLUE = 0x001F
WHITE = 0xFFFF
YELLOW = 0xFFE0


def border_frame() -> bytes:
    px = [[BLACK] * WIDTH for _ in range(HEIGHT)]

    for x in range(WIDTH):
        px[0][x] = RED
        px[HEIGHT - 1][x] = GREEN
    for y in range(HEIGHT):
        px[y][0] = BLUE
        px[y][WIDTH - 1] = WHITE

    # Orientation tick: 8 x 8 solid block just inside the top-left corner.
    for y in range(1, 9):
        for x in range(1, 9):
            px[y][x] = YELLOW

    out = bytearray()
    for row in px:
        for word in row:
            out += struct.pack(">H", word)
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=1_000_000,
                    help="ignored by the board, but 1200 reboots it into BOOTSEL")
    args = ap.parse_args()

    if args.baud == 1200:
        print("refusing --baud 1200: that reboots the board into BOOTSEL", file=sys.stderr)
        return 2

    frame = border_frame()
    with serial.Serial(args.port, args.baud, timeout=2) as ser:
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.write(HEADER + frame)
        ser.flush()
        ack = ser.read(1)

    print("sent border frame" + (", acknowledged" if ack == b"K" else ", NO ack"))
    print(__doc__.split("Each edge")[1].split("All four")[0].strip())
    print("all four lines visible, corners meeting  ->  offsets correct")
    return 0


if __name__ == "__main__":
    sys.exit(main())
