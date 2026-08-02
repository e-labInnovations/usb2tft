
# USB2TFT

Firmware for an RP2040 Zero driving a salvaged 128 × 128 ST7735S RGB565 display over SPI. The board enumerates as a USB CDC serial device and paints whatever frames the host sends it, fast enough for live video.

The panel came out of a reverse-engineered smartwatch. Its original TLSR8232 was replaced with an RP2040 Zero, because the TLSR8232's UART capped the link at about 6 fps.

## Wiring

| Display signal | RP2040 Zero GPIO | Notes |
| --- | ---: | --- |
| SDA | GP7 | SPI0 TX (MOSI) |
| SCL | GP2 | SPI0 SCK |
| RS | GP4 | Data/command select |
| RES | GP0 | Reset |
| CS | GP5 | Active-low chip select |
| VCC | 3.3 V | Do not use 5 V logic |
| GND | GND | Common ground |
| LEDA | Backlight supply | Connect as required by the display's backlight circuit |

The display's `LEDA`/`LEDK` pins are for the backlight and are separate from its SPI interface. GP3 is dead on this board, and GP6 was the old bit-banged MOSI before the move to hardware SPI0.

## Which port

On Linux the board is usually `/dev/ttyACM0`, but that number shifts as soon as another CDC device is plugged in. Prefer the stable path, which is tied to the descriptors in `usb_descriptors.c`:

```bash
PORT=/dev/serial/by-id/usb-e-lab_innovations_USB2TFT_000001-if00
```

`ls /dev/serial/by-id/` lists what is attached. The examples below assume `$PORT` is set.

## Host requirements

```bash
python3 -m pip install Pillow pyserial   # both senders
```

`tools/play_video.py` also needs `ffmpeg` on `PATH`.

## Sending still images and GIFs

`tools/send_frame.py` converts with Pillow and needs no ffmpeg. Use it for stills and short animations.

```bash
python3 tools/send_frame.py $PORT image.png
python3 tools/send_frame.py $PORT animation.gif --loop
```

| Option | |
| --- | --- |
| `--fps N` | Override the source frame rate. By default each GIF frame is held for its own embedded duration, floored at 10 ms. |
| `--loop` | Repeat the animation, or resend a still image, forever. |
| `--baud N` | Line coding for the CDC port. Default 115200. Does not affect speed — the link is USB underneath. |

Any format Pillow can open works. Frames are scaled to cover the panel and centre-cropped, then converted to big-endian RGB565; there is no letterbox option here, unlike `play_video.py --fit`. Multi-frame files are decoded up front, so a long GIF costs memory and a pause before playback starts.

> **Do not pass `--baud 1200`.** The firmware treats 1200 baud as the reboot-to-BOOTSEL signal, so the board will drop off the bus instead of displaying anything.

Conversion is a per-pixel Python loop, which is fine for stills but far too slow to feed video — that is what `play_video.py` and ffmpeg are for.

## Playing live video

`tools/play_video.py` hands decoding, scaling and RGB565 conversion to `ffmpeg` and only relays finished frames, so it keeps up where a per-pixel Python loop cannot.

```bash
python3 tools/play_video.py $PORT video.mp4
python3 tools/play_video.py $PORT video.mp4 --loop
```

Any ffmpeg input works, so live sources need nothing extra beyond telling ffmpeg what they are:

```bash
python3 tools/play_video.py $PORT /dev/video0 --format v4l2   # webcam
python3 tools/play_video.py $PORT :0.0 --format x11grab       # mirror the desktop
```

| Option | |
| --- | --- |
| `--fps N` | Frames per second to send. Default 16, which is the link's ceiling. |
| `--fit crop\|pad\|stretch` | How a non-square source meets the square panel. Default `crop`. |
| `--format F` | ffmpeg input format, for sources that are not files. |
| `--loop` | Repeat the source forever. |
| `--no-drop` | Send every frame and let playback lag, instead of skipping late ones. |

Any resolution, frame rate, codec and container ffmpeg can open will play. The source rate is resampled to the send rate, so a 5 fps clip has frames duplicated and a 60 fps clip has them dropped, both at correct speed; variable-rate sources are normalised the same way. Anamorphic pixel aspect ratios are corrected rather than squashed.

`--fit` matters only for non-square sources:

| | |
| --- | --- |
| `crop` (default) | Fill the panel, centre-crop the overflow. Loses about 44% of the width of a 16:9 source. |
| `pad` | Letterbox. Whole frame visible, black bars on the short axis. |
| `stretch` | Ignore aspect ratio. |

When the link falls behind, frames are skipped so playback tracks the wall clock instead of drifting; `--no-drop` sends everything and lets playback lag behind instead. On exit — including on Ctrl-C — it prints the frame rate it actually achieved.

Audio is discarded. HDR and 10-bit sources play but are not tone-mapped, so they look washed out. A 4K source is limited by host decode speed rather than by the link.

Test clips live in `tools/assets/`, which is deliberately untracked.

## Frame protocol

Each frame is an 8-byte header followed by 128 × 128 RGB565 pixels in big-endian order:

```text
TFT1 + 0x00008000 + RGB565 pixel data
```

The header is resynchronising: the reader scans for `TFT1` and the payload length, so a sender that dies mid-frame costs one frame, not the stream. The board replies with one `K` byte per frame it has finished painting, which lets a sender pace itself without guessing.

## How the firmware works

`main.c` runs a single loop on core 0:

1. `tud_task()` services TinyUSB.
2. Received bytes are bulk-copied straight from the CDC FIFO into a framebuffer. Only the 8 header bytes are read one at a time.
3. A completed frame is handed to DMA, which streams it to SPI0 while the next frame is received into the *other* framebuffer.
4. When DMA finishes, CS is released and one ack byte is queued.

Two 32 KiB framebuffers live in static RAM. A single frame overflows the RP2040's default main stack, which is what blocked USB enumeration during bring-up.

`SPI_HZ` is 31.25 MHz — the RP2040 divides its 125 MHz peripheral clock, so the reachable rates are 125/2, 125/4, 125/6 … MHz. Drop to `15625000` if long or unshielded wiring produces speckled pixels.

`pico_fix_rp2040_usb_device_enumeration` is deliberately **not** enabled. That workaround targets the RP2040-E5 erratum, and with it the device enumerated and then dropped its link on this silicon.

## Build

This project uses the Raspberry Pi Pico SDK. Configure it once, then build:

```bash
cmake -S . -B build -DPICO_SDK_PATH="$PICO_SDK_PATH"
cmake --build build
```

The firmware image is written to `build/usb2tft.uf2`.

## Flash

Opening the CDC port at 1200 baud reboots the board into BOOTSEL, so reflashing needs no button press:

```bash
cmake --build build
python3 -c "import serial; serial.Serial('$PORT', 1200).close()"
cp build/usb2tft.uf2 /media/$USER/RPI-RP2/
```

Give the desktop a few seconds to automount `RPI-RP2` before copying. If it does not appear, mount it manually with `udisksctl mount -b /dev/sda1`.

For a first flash, or to recover a board that is not enumerating:

1. Hold the RP2040 Zero's **BOOTSEL** button while connecting USB.
2. It appears as a removable drive named `RPI-RP2`.
3. Copy `build/usb2tft.uf2` to that drive.
4. The board restarts automatically.

## Throughput

Measured end to end on an x86 Linux host:

| | |
| --- | --- |
| USB CDC bulk OUT | ~512 KiB/s |
| Frame size | 32,768 bytes |
| Sustained frame rate | **~16 fps** |

A 152-second 360 × 360 clip sent 2408 frames at 15.7 fps, 2407 of them acknowledged, 1% dropped, finishing 1.6 s behind a 152.0 s source — no drift and no disconnect.

USB Full Speed is the limit, not the panel:

- Batching 1, 4 or 16 frames per `write()` all measure ~512 KiB/s, so host write boundaries are not the cost.
- With panel writes removed from the firmware entirely, throughput is unchanged at ~512 KiB/s.
- DMA occupies 8.4 ms of a 62.5 ms frame, about 2% of the budget once the transfer overlaps reception.

~512 KiB/s is roughly 43% of Full Speed's theoretical 1.2 MB/s, which is the usual figure for a single non-double-buffered RP2040 bulk endpoint. Going faster therefore means sending fewer bytes — 12-bit RGB444 via `COLMOD 0x03` would buy about 1.33×, half-resolution with pixel doubling on the device rather more — or leaving CDC for a vendor bulk class with alternating endpoints. Optimising the display path further would buy nothing.

Compression is not the answer here: RLE and inter-frame delta both need flat or static content, and video is neither.

## Current status

- ST7735S initialization: working
- SPI display writes: working, DMA at 31.25 MHz with double buffering
- USB CDC RGB565 framebuffer protocol: working
- Still images and animated GIFs: working
- Live video, webcam and desktop mirroring: working at ~16 fps
- Reflash without the BOOTSEL button: working
