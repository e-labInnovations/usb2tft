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


def parse_time(text: str) -> float:
    """Seconds from ``90``, ``1:30``, ``01:02:03`` or ``1:02:03.5``."""
    parts = text.strip().split(":")
    if len(parts) > 3:
        raise ValueError(f"not a time: {text}")
    seconds = 0.0
    for part in parts:
        seconds = seconds * 60 + float(part)
    if seconds < 0:
        raise ValueError("time may not be negative")
    return seconds


def format_time(seconds: float) -> str:
    seconds = int(seconds)
    hours, seconds = divmod(seconds, 3600)
    minutes, seconds = divmod(seconds, 60)
    if hours:
        return f"{hours}:{minutes:02d}:{seconds:02d}"
    return f"{minutes}:{seconds:02d}"


def probe_duration(args):
    """Source length in seconds, or None when ffprobe cannot say."""
    if shutil.which("ffprobe") is None:
        return None
    command = ["ffprobe", "-v", "error"]
    if args.format:
        command += ["-f", args.format]
    command += ["-show_entries", "format=duration",
                "-of", "default=noprint_wrappers=1:nokey=1", args.source]
    try:
        output = subprocess.run(command, capture_output=True, text=True,
                                timeout=10).stdout.strip()
        return float(output)
    except (subprocess.SubprocessError, ValueError):
        # Live sources and streams have no duration; that is not an error.
        return None


def ffmpeg_command(args) -> list:
    command = ["ffmpeg", "-hide_banner", "-loglevel", "error"]
    if args.loop:
        command += ["-stream_loop", "-1"]
    if args.format:
        command += ["-f", args.format]
    if args.start:
        # Before -i: ffmpeg seeks the input instead of decoding and discarding
        # everything up to the mark, which on a long file is the difference
        # between instant and a minute of waiting.
        command += ["-ss", str(args.start)]
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


def ffplay_command(args) -> list:
    """Play the source's audio track on this computer, no window."""
    command = ["ffplay", "-hide_banner", "-loglevel", "error",
               "-nodisp", "-autoexit", "-vn"]
    if args.loop:
        command += ["-loop", "0"]
    if args.format:
        command += ["-f", args.format]
    if args.start:
        command += ["-ss", str(args.start)]
    command += ["-volume", str(args.volume), "-i", args.source]
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
    parser.add_argument("--audio", action="store_true",
                        help="Play the source's audio on this computer via ffplay")
    parser.add_argument("--volume", type=int, default=100,
                        help="Audio volume 0-100 for --audio (default: 100)")
    parser.add_argument("--no-drop", action="store_true",
                        help="Never skip frames, even when the link falls behind")
    parser.add_argument("--start", default="0", metavar="TIME",
                        help="Begin at this point: seconds, or [HH:]MM:SS (default: 0)")
    parser.add_argument("--no-progress", action="store_true",
                        help="Do not print the live position line")
    args = parser.parse_args()

    if args.fps <= 0:
        parser.error("--fps must be greater than zero")
    try:
        args.start = parse_time(args.start)
    except ValueError:
        parser.error(f"--start: not a time: {args.start!r}")
    if not 0 <= args.volume <= 100:
        parser.error("--volume must be between 0 and 100")
    if shutil.which("ffmpeg") is None:
        print("ffmpeg not found on PATH.", file=sys.stderr)
        return 1
    if args.audio and shutil.which("ffplay") is None:
        print("ffplay not found on PATH (it ships with ffmpeg).", file=sys.stderr)
        return 1

    period = 1.0 / args.fps
    sent = 0
    dropped = 0
    acked = 0
    decoded = 0

    # A live source has no length; then the line shows position only.
    duration = probe_duration(args) if not args.no_progress else None
    if duration is not None and not args.loop and args.start >= duration:
        print(f"--start {format_time(args.start)} is past the end of the source "
              f"({format_time(duration)}).", file=sys.stderr)
        return 1
    progress = not args.no_progress and sys.stderr.isatty()
    total = f" / {format_time(duration)}" if duration is not None else ""

    def show_progress():
        """One line, rewritten in place — \\r, no newline, no curses."""
        position = args.start + decoded * period
        if duration is not None and args.loop and position > duration:
            # Wrap so a looping source reads as the source's own clock.
            span = max(duration - args.start, period)
            position = args.start + (position - args.start) % span
        rate = sent / elapsed if (elapsed := time.monotonic() - start) > 0 else 0.0
        sys.stderr.write(f"\r  {format_time(position)}{total}   {rate:5.1f} fps"
                         f"   {dropped} dropped   ")
        sys.stderr.flush()

    # Turn a kill into an ordinary unwind so ffmpeg is shut down before we stop
    # reading it; otherwise it dies complaining about a broken pipe.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    ffmpeg = subprocess.Popen(ffmpeg_command(args), stdout=subprocess.PIPE)
    ffplay = None
    start = time.monotonic()
    try:
        with serial.Serial(args.port, 115200, timeout=0, write_timeout=5) as device:
            # Opening a CDC port can toggle DTR. Let the RP2040 finish USB setup.
            time.sleep(0.25)
            device.reset_input_buffer()

            start = time.monotonic()
            deadline = start
            next_progress = start
            while True:
                frame = read_exact(ffmpeg.stdout, FRAME_BYTES)
                if frame is None:
                    break
                decoded += 1

                if progress and time.monotonic() >= next_progress:
                    show_progress()
                    next_progress = time.monotonic() + 0.25

                if args.audio and ffplay is None:
                    # Start on the first decoded frame, not at spawn time: that
                    # is when video actually begins, so the two stay in step.
                    ffplay = subprocess.Popen(ffplay_command(args),
                                              stdin=subprocess.DEVNULL)
                    start = deadline = next_progress = time.monotonic()

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
        if progress and decoded:
            show_progress()
            sys.stderr.write("\n")
        # SIGKILL, not SIGTERM: on SIGTERM ffmpeg tries to flush its output and
        # blocks writing to a pipe we have stopped reading, so wait() deadlocks.
        if ffplay is not None and ffplay.poll() is None:
            ffplay.terminate()
            ffplay.wait()
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
