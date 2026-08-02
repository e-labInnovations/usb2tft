#!/usr/bin/env python3
"""Stream live video to USB2TFT over its USB CDC port.

ffmpeg does the decoding, scaling and RGB565 conversion, so this script only
relays finished frames to the board. Anything ffmpeg can read works as a source:
a video file, a webcam (``-f v4l2 /dev/video0``) or the desktop
(``-f x11grab :0.0``).
"""

import argparse
import shutil
import signal
import struct
import subprocess
import sys
import time

import serial

WIDTH = 128
HEIGHT = 128
FRAME_BYTES = WIDTH * HEIGHT * 2
HEADER = b"TFT1" + struct.pack(">I", FRAME_BYTES)
FRAME_ACK = b"K"


# How to reconcile a source of any shape with the square panel.
FIT_FILTERS = {
    # Cover the panel and centre-crop the overflow. Matches send_frame.py.
    "crop": (f"scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=increase,"
             f"crop={WIDTH}:{HEIGHT}"),
    # Letterbox: whole frame visible, black bars on the short axis.
    "pad": (f"scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=decrease,"
            f"pad={WIDTH}:{HEIGHT}:(ow-iw)/2:(oh-ih)/2:black"),
    # Ignore aspect ratio entirely.
    "stretch": f"scale={WIDTH}:{HEIGHT}",
}


def ffmpeg_command(args) -> list:
    command = ["ffmpeg", "-hide_banner", "-loglevel", "error"]
    if args.loop:
        command += ["-stream_loop", "-1"]
    if args.format:
        command += ["-f", args.format]
    command += ["-i", args.source]
    # fps= resamples any source rate, constant or variable, to ours.
    # setsar=1 keeps anamorphic sources from coming out squashed.
    command += [
        "-vf", f"fps={args.fps},{FIT_FILTERS[args.fit]},setsar=1",
        "-an",
        "-f", "rawvideo",
        "-pix_fmt", "rgb565be",
        "-",
    ]
    return command


def read_exact(stream, count: int):
    """Read exactly *count* bytes, or return None at end of stream."""
    chunks = []
    remaining = count
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            return None
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB CDC port, e.g. /dev/ttyACM0")
    parser.add_argument("source", help="Video file, or any ffmpeg input")
    parser.add_argument("--fps", type=float, default=16.0,
                        help="Frames per second to send (default: 16, the USB CDC ceiling)")
    parser.add_argument("--fit", choices=sorted(FIT_FILTERS), default="crop",
                        help="Fit a non-square source to the panel (default: crop)")
    parser.add_argument("--format", help="ffmpeg input format, e.g. v4l2 or x11grab")
    parser.add_argument("--loop", action="store_true", help="Repeat the source forever")
    parser.add_argument("--no-drop", action="store_true",
                        help="Never skip frames, even when the link falls behind")
    args = parser.parse_args()

    if args.fps <= 0:
        parser.error("--fps must be greater than zero")
    if shutil.which("ffmpeg") is None:
        print("ffmpeg not found on PATH.", file=sys.stderr)
        return 1

    period = 1.0 / args.fps
    sent = 0
    dropped = 0
    acked = 0

    # Turn a kill into an ordinary unwind so ffmpeg is shut down before we stop
    # reading it; otherwise it dies complaining about a broken pipe.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    ffmpeg = subprocess.Popen(ffmpeg_command(args), stdout=subprocess.PIPE)
    start = time.monotonic()
    try:
        with serial.Serial(args.port, 115200, timeout=0, write_timeout=5) as device:
            # Opening a CDC port can toggle DTR. Let the RP2040 finish USB setup.
            time.sleep(0.25)
            device.reset_input_buffer()

            start = time.monotonic()
            deadline = start
            while True:
                frame = read_exact(ffmpeg.stdout, FRAME_BYTES)
                if frame is None:
                    break

                deadline += period
                now = time.monotonic()
                if now < deadline:
                    time.sleep(deadline - now)
                elif not args.no_drop and now > deadline + period:
                    # More than one period behind: skip this frame so playback
                    # tracks wall clock instead of drifting further back.
                    dropped += 1
                    deadline = now
                    continue

                device.write(HEADER)
                device.write(frame)
                sent += 1

                # Count the board's per-frame acknowledgements without blocking.
                waiting = device.in_waiting
                if waiting:
                    acked += device.read(waiting).count(FRAME_ACK)
    except KeyboardInterrupt:
        pass
    finally:
        # SIGKILL, not SIGTERM: on SIGTERM ffmpeg tries to flush its output and
        # blocks writing to a pipe we have stopped reading, so wait() deadlocks.
        if ffmpeg.poll() is None:
            ffmpeg.kill()
        if ffmpeg.stdout:
            ffmpeg.stdout.close()
        ffmpeg.wait()

        elapsed = time.monotonic() - start
        if elapsed > 0:
            print(f"sent {sent} frames in {elapsed:.1f}s "
                  f"({sent / elapsed:.1f} fps), {dropped} dropped, "
                  f"{acked} acknowledged by the board")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
