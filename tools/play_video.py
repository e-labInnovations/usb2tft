#!/usr/bin/env python3
"""Stream live video to USB2TFT over its USB CDC port.

ffmpeg does the decoding, scaling and RGB565 conversion, so this script only
relays finished frames to the board. Anything ffmpeg can read works as a source:
a video file, a webcam (``-f v4l2 /dev/video0``) or the desktop
(``-f x11grab :0.0``).
"""

import argparse
import os
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time

import serial

WIDTH = 128
HEIGHT = 128
FRAME_BYTES = WIDTH * HEIGHT * 2
HEADER = b"TFT1" + struct.pack(">I", FRAME_BYTES)
FRAME_ACK = b"K"

# Audio shares the one bulk endpoint with video, so every byte of sound is a
# byte of picture.  s16le mono at 22.05 kHz is 44,100 B/s against a measured
# ceiling of 512 KiB/s, about 8% of the bus, which shows up as a lower frame
# rate rather than as dropped audio: the board plays silence when it runs dry,
# and silence is the one failure everybody hears.
AUDIO_RATE = 22050
AUDIO_BYTES_PER_SECOND = AUDIO_RATE * 2
AUDIO_CHUNK = 1024                      # 23 ms
AUDIO_HEADER = b"PCM1"
UNDERRUN_ACK = b"U"
OVERFLOW_ACK = b"O"
RESTART_ACK = b"R"


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
        "-map", "0:v:0",
        "-vf", f"fps={args.fps},{FIT_FILTERS[args.fit]},setsar=1",
        "-an",
        "-f", "rawvideo",
        "-pix_fmt", "rgb565be",
        "-",
    ]
    # One process decodes once and writes both streams, video on fd 3 and audio
    # on fd 4.  Two processes meant decoding the same file twice, and on a
    # laptop that competition cost more frame rate than the audio bandwidth did.
    if args.audio:
        # pipe:N takes the real descriptor number, which is whatever the OS
        # handed us: pass_fds keeps a descriptor open across the fork but does
        # not renumber it, so hardcoding 4 gets "Bad file descriptor".
        command += audio_output_args(args, f"pipe:{args.audio_fd}")
    return command


def audio_output_args(args, target: str) -> list:
    """The audio half of the ffmpeg command line, written to *target*."""
    out = ["-map", "0:a:0",
           "-vn", "-ac", "1", "-ar", str(AUDIO_RATE),
           "-f", "s16le", "-acodec", "pcm_s16le"]
    if args.volume != 100:
        out += ["-filter:a", f"volume={args.volume / 100:.3f}"]
    return out + [target]


def has_audio_stream(args) -> bool:
    """False for a webcam, a screen grab, or a file with no audio track."""
    if shutil.which("ffprobe") is None:
        return True
    command = ["ffprobe", "-v", "error"]
    if args.format:
        command += ["-f", args.format]
    command += ["-select_streams", "a", "-show_entries", "stream=index",
                "-of", "csv=p=0", args.source]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        return bool(result.stdout.strip())
    except subprocess.SubprocessError:
        return True


class AudioReader:
    """Decoded audio, read on a thread so the video path never blocks on it.

    A blocking pipe read in the middle of the send loop was costing a third of
    the frame rate: ffmpeg would be busy with a video packet, the read would
    stall, and the board's ring would run dry while the loop waited.
    """

    LIMIT = 1 << 20   # 24 s of audio; keeps a long clip from growing unbounded

    def __init__(self, stream):
        self._stream = stream
        self._buffer = bytearray()
        self._lock = threading.Lock()
        self._room = threading.Event()
        self._room.set()
        self.eof = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while True:
            self._room.wait()
            data = self._stream.read(AUDIO_CHUNK * 8)
            if not data:
                self.eof = True
                return
            with self._lock:
                self._buffer += data
                if len(self._buffer) > self.LIMIT:
                    self._room.clear()

    def take(self, count: int) -> bytes:
        """Up to *count* bytes, or b"" when nothing is buffered yet."""
        with self._lock:
            chunk = bytes(self._buffer[:count])
            del self._buffer[:len(chunk)]
            if len(self._buffer) <= self.LIMIT // 2:
                self._room.set()
        return chunk


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
    parser.add_argument("--fps", type=float, default=None,
                        help="Frames per second to send (default: 16, or 14 with "
                             "--audio, which is what the bus has left)")
    parser.add_argument("--fit", choices=sorted(FIT_FILTERS), default="crop",
                        help="Fit a non-square source to the panel (default: crop)")
    parser.add_argument("--format", help="ffmpeg input format, e.g. v4l2 or x11grab")
    parser.add_argument("--loop", action="store_true", help="Repeat the source forever")
    parser.add_argument("--audio", action="store_true",
                        help="Send the source's audio to the board's speaker")
    parser.add_argument("--audio-host", action="store_true",
                        help="Play the audio on this computer via ffplay instead")
    parser.add_argument("--volume", type=int, default=100,
                        help="Audio volume 0-100 (default: 100)")
    parser.add_argument("--lead", type=float, default=150.0, metavar="MS",
                        help="Audio buffered ahead of real time, and the amount "
                             "the video is delayed to match (default: 150)")
    parser.add_argument("--no-drop", action="store_true",
                        help="Never skip frames, even when the link falls behind")
    parser.add_argument("--start", default="0", metavar="TIME",
                        help="Begin at this point: seconds, or [HH:]MM:SS (default: 0)")
    parser.add_argument("--no-progress", action="store_true",
                        help="Do not print the live position line")
    args = parser.parse_args()

    if args.fps is None:
        # Audio and video share one saturated endpoint.  512 KiB/s less the
        # 44 KiB/s the samples need leaves 14.3 frames, and asking for 16
        # anyway does not get them: the writes fall behind, frames drop, and
        # the jitter that causes is what starves the board's audio ring.
        # Measured: 16 asked gives 12.4 fps with 63 underruns, 14 asked gives
        # 13.8 fps with 8 and nothing dropped.
        args.fps = 14.0 if args.audio else 16.0
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
    if args.audio and args.audio_host:
        parser.error("--audio and --audio-host are exclusive")
    if args.audio and not has_audio_stream(args):
        print("source has no audio track; sending video only", file=sys.stderr)
        args.audio = False
    if args.lead < 0:
        parser.error("--lead may not be negative")
    if args.audio_host and shutil.which("ffplay") is None:
        print("ffplay not found on PATH (it ships with ffmpeg).", file=sys.stderr)
        return 1

    period = 1.0 / args.fps
    sent = 0
    dropped = 0
    acked = 0
    decoded = 0
    audio_sent = 0
    underruns = 0
    overflows = 0
    restarts = 0

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

    if args.audio:
        audio_read, audio_write = os.pipe()
        args.audio_fd = audio_write
        ffmpeg = subprocess.Popen(ffmpeg_command(args), stdout=subprocess.PIPE,
                                  pass_fds=(audio_write,))
        os.close(audio_write)
        audio = AudioReader(os.fdopen(audio_read, "rb", 0))
    else:
        ffmpeg = subprocess.Popen(ffmpeg_command(args), stdout=subprocess.PIPE)
        audio = None
    video_stream = ffmpeg.stdout
    ffplay = None
    start = time.monotonic()
    try:
        with serial.Serial(args.port, 115200, timeout=0, write_timeout=5) as device:
            # Opening a CDC port can toggle DTR. Let the RP2040 finish USB setup.
            time.sleep(0.25)
            device.reset_input_buffer()

            start = time.monotonic()
            pending = []
            # Video is held back by the audio lead so the two line up: the
            # board is deliberately playing sound from `lead` milliseconds ago.
            deadline = start + (args.lead / 1000.0 if audio else 0.0)
            next_progress = start

            def due_audio() -> list:
                """Chunks the board should have by now, as wire-ready bytes.

                Nothing here blocks or writes: what the reader thread has not
                produced yet is simply not sent, and the caller decides when the
                bytes go out.  Writing flat out instead of on a schedule fills
                the board's ring in the first second and everything after it is
                dropped.
                """
                nonlocal audio_sent
                parts = []
                while True:
                    due = start + audio_sent / AUDIO_BYTES_PER_SECOND - args.lead / 1000.0
                    if time.monotonic() < due:
                        return parts
                    chunk = audio.take(AUDIO_CHUNK)
                    if not chunk:
                        return parts
                    parts.append(AUDIO_HEADER + struct.pack(">I", len(chunk)))
                    parts.append(chunk)
                    audio_sent += len(chunk)

            while True:
                frame = read_exact(video_stream, FRAME_BYTES)
                if frame is None:
                    break
                decoded += 1

                if progress and time.monotonic() >= next_progress:
                    show_progress()
                    next_progress = time.monotonic() + 0.25

                if args.audio_host and ffplay is None:
                    # Start on the first decoded frame, not at spawn time: that
                    # is when video actually begins, so the two stay in step.
                    ffplay = subprocess.Popen(ffplay_command(args),
                                              stdin=subprocess.DEVNULL)
                    start = deadline = next_progress = time.monotonic()

                deadline += period
                now = time.monotonic()
                if now < deadline:
                    # Audio is not idle time: fill the wait with whatever the
                    # board is due, then sleep out the remainder.
                    if audio is not None:
                        pending += due_audio()
                        now = time.monotonic()
                    if now < deadline:
                        time.sleep(deadline - now)
                elif not args.no_drop and now > deadline + period:
                    # More than one period behind: skip this frame so playback
                    # tracks wall clock instead of drifting further back.
                    dropped += 1
                    deadline = now
                    continue

                # One write per frame, with the audio that is due in front of
                # it.  Each write on a tty costs a couple of milliseconds of
                # fixed latency, so nine of them per frame (a sliced frame plus
                # its audio) cost about 25 ms and a third of the frame rate.
                # The audio jitter this introduces is one frame period, well
                # inside the board's 372 ms ring.
                if audio is not None:
                    pending += due_audio()
                pending.append(HEADER)
                pending.append(frame)
                device.write(b"".join(pending))
                pending.clear()
                sent += 1

                # Count the board's replies without blocking.
                waiting = device.in_waiting
                if waiting:
                    replies = device.read(waiting)
                    acked += replies.count(FRAME_ACK)
                    underruns += replies.count(UNDERRUN_ACK)
                    overflows += replies.count(OVERFLOW_ACK)
                    restarts += replies.count(RESTART_ACK)
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
        try:
            video_stream.close()
        except OSError:
            pass
        ffmpeg.wait()

        elapsed = time.monotonic() - start
        if elapsed > 0:
            summary = (f"sent {sent} frames in {elapsed:.1f}s "
                       f"({sent / elapsed:.1f} fps), {dropped} dropped, "
                       f"{acked} acknowledged by the board")
            if audio is not None:
                summary += (f"; {audio_sent / AUDIO_BYTES_PER_SECOND:.1f}s of audio, "
                            f"{underruns} underruns, {overflows} overflows, "
                            f"{restarts} chain restarts")
            print(summary)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
