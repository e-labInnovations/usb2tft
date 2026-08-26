#!/usr/bin/env python3
"""Measure USB2TFT's real audio sample clock, and read its fault counters.

A wrong PIO divider does not sound like a clocking fault, it sounds like bad
audio, so the only way to tell a correct divider from a plausible one is to
count frames over a known interval. The board reports how many frames it has
handed to the I2S engine; two readings and a stopwatch give the rate.
"""

import argparse
import struct
import sys
import time

import serial

MAGIC_STAT = b"STAT" + struct.pack(">I", 0)
NOMINAL_RATE = 22050


def read_stat(device, timeout=2.0):
    """Ask for counters and return (frames, underruns, overflows, restarts)."""
    device.write(MAGIC_STAT)
    device.flush()
    deadline = time.monotonic() + timeout
    buffer = bytearray()
    while time.monotonic() < deadline:
        buffer += device.read(64)
        # Single-byte acks ('K', 'U', 'O', 'R') share the stream, so find the
        # reply by its marker rather than assuming it arrives first.
        index = buffer.find(b"S")
        if index >= 0 and len(buffer) >= index + 17:
            return struct.unpack(">IIII", buffer[index + 1:index + 17])
        time.sleep(0.01)
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("port")
    ap.add_argument("--seconds", type=float, default=5.0,
                    help="Interval between the two readings (default: 5)")
    args = ap.parse_args()

    with serial.Serial(args.port, 115200, timeout=0.1) as device:
        time.sleep(0.25)
        device.reset_input_buffer()

        first = read_stat(device)
        if first is None:
            print("no reply: firmware without STAT support?", file=sys.stderr)
            return 1
        started = time.monotonic()
        time.sleep(args.seconds)
        second = read_stat(device)
        if second is None:
            print("no reply to the second request", file=sys.stderr)
            return 1
        elapsed = time.monotonic() - started

    frames = second[0] - first[0]
    rate = frames / elapsed
    print(f"{frames} frames in {elapsed:.2f}s")
    print(f"measured sample clock  {rate:9.1f} Hz")
    print(f"nominal                {NOMINAL_RATE:9.1f} Hz"
          f"   ({rate / NOMINAL_RATE:.3f}x)")
    print(f"underruns {second[1] - first[1]}, overflows {second[2] - first[2]}, "
          f"chain restarts {second[3] - first[3]} during the interval")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
