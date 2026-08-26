#!/usr/bin/env python3
"""Stream audio to USB2TFT's speaker over its USB CDC port.

ffmpeg does the decoding and the resampling, so this script only paces finished
samples onto the wire. Anything ffmpeg can read works as a source.

The board buffers a few hundred milliseconds and plays silence if it runs dry,
so the job here is to stay slightly ahead of real time without getting so far
ahead that the ring overflows. Both faults are reported back by the board a
byte at a time and printed in the summary, because an underrun you can only
hear is an underrun you cannot count.
"""

import argparse
import shutil
import signal
import struct
import subprocess
import sys
import time

import serial

SAMPLE_RATE = 22050
BYTES_PER_SAMPLE = 2
CHUNK_BYTES = 1024           # 23 ms of audio, 2 ms of bus time
MAGIC_PCM = b"PCM1"

FRAME_ACK = ord("K")
UNDERRUN = ord("U")
OVERFLOW = ord("O")
RESTART = ord("R")


def ffmpeg_command(args) -> list:
    command = ["ffmpeg", "-hide_banner", "-loglevel", "error"]
    if args.loop:
        command += ["-stream_loop", "-1"]
    if args.start:
        command += ["-ss", str(args.start)]
    command += ["-i", args.source,
                "-vn",
                "-ac", "1",
                "-ar", str(SAMPLE_RATE),
                "-f", "s16le",
                "-acodec", "pcm_s16le"]
    if args.volume != 100:
        command += ["-filter:a", f"volume={args.volume / 100:.3f}"]
    command += ["-"]
    return command


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB CDC port, e.g. /dev/ttyACM0")
    parser.add_argument("source", help="Audio or video file, or any ffmpeg input")
    parser.add_argument("--lead", type=float, default=150.0, metavar="MS",
                        help="How far ahead of real time to stay (default: 150)")
    parser.add_argument("--volume", type=int, default=100,
                        help="Scale the samples, 0-100 (default: 100)")
    parser.add_argument("--loop", action="store_true", help="Repeat the source forever")
    parser.add_argument("--start", default="0", metavar="SECONDS",
                        help="Begin at this offset (default: 0)")
    args = parser.parse_args()

    if not 0 <= args.volume <= 100:
        parser.error("--volume must be between 0 and 100")
    if args.lead < 0:
        parser.error("--lead may not be negative")
    if shutil.which("ffmpeg") is None:
        print("ffmpeg not found on PATH.", file=sys.stderr)
        return 1

    bytes_per_second = SAMPLE_RATE * BYTES_PER_SAMPLE
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    ffmpeg = subprocess.Popen(ffmpeg_command(args), stdout=subprocess.PIPE)
    sent = 0
    underruns = 0
    overflows = 0
    restarts = 0
    start = time.monotonic()

    try:
        with serial.Serial(args.port, 115200, timeout=0, write_timeout=5) as device:
            # Opening a CDC port can toggle DTR. Let the RP2040 settle.
            time.sleep(0.25)
            device.reset_input_buffer()
            start = time.monotonic()

            while True:
                chunk = ffmpeg.stdout.read(CHUNK_BYTES)
                if not chunk:
                    break

                # Hold back until the board is within `lead` of needing this
                # chunk. Without this the ring fills in the first second and
                # every chunk after that is dropped.
                due = start + sent / bytes_per_second - args.lead / 1000.0
                now = time.monotonic()
                if now < due:
                    time.sleep(due - now)

                device.write(MAGIC_PCM + struct.pack(">I", len(chunk)))
                device.write(chunk)
                sent += len(chunk)

                waiting = device.in_waiting
                if waiting:
                    replies = device.read(waiting)
                    underruns += replies.count(bytes([UNDERRUN]))
                    overflows += replies.count(bytes([OVERFLOW]))
                    restarts += replies.count(bytes([RESTART]))
    except KeyboardInterrupt:
        pass
    finally:
        # SIGKILL, not SIGTERM: on SIGTERM ffmpeg tries to flush into a pipe
        # nobody is reading and wait() deadlocks.
        if ffmpeg.poll() is None:
            ffmpeg.kill()
        if ffmpeg.stdout:
            ffmpeg.stdout.close()
        ffmpeg.wait()

        elapsed = time.monotonic() - start
        seconds = sent / bytes_per_second
        print(f"sent {seconds:.1f}s of audio in {elapsed:.1f}s "
              f"({sent / 1024:.0f} KiB, {underruns} underruns, "
              f"{overflows} overflows, {restarts} chain restarts)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
