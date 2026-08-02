
# USB2TFT

Firmware for an RP2040 Zero driving a salvaged 128 × 128 ST7735S RGB565 display over SPI. The board enumerates as a USB CDC serial device and paints whatever frames the host sends it, fast enough for live video.

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

The display's `LEDA`/`LEDK` pins are for the backlight and are separate from its SPI interface.

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
python3 -c "import serial; serial.Serial('/dev/ttyACM0', 1200).close()"
cp build/usb2tft.uf2 /media/$USER/RPI-RP2/
```

For a first flash, or to recover a board that is not enumerating:

1. Hold the RP2040 Zero's **BOOTSEL** button while connecting USB.
2. It appears as a removable drive named `RPI-RP2`.
3. Copy `build/usb2tft.uf2` to that drive.
4. The board restarts automatically.

## Frame protocol

Each frame is an 8-byte header followed by 128 × 128 RGB565 pixels in big-endian order:

```text
TFT1 + 0x00008000 + RGB565 pixel data
```

The board replies with one `K` byte per frame it has finished painting, which lets a sender pace itself without guessing.

## Sending still images

Needs `Pillow` and `pyserial`:

```bash
python3 -m pip install Pillow pyserial
python3 tools/send_frame.py /dev/ttyACM0 image.png
```

The sender resizes images to fit the screen, centre-crops them, converts them to RGB565, and streams the result. Animated GIFs are sent at their embedded frame rate (or use `--fps` to override it):

```bash
python3 tools/send_frame.py /dev/ttyACM0 animation.gif --loop
```

## Playing live video

`tools/play_video.py` hands decoding, scaling and RGB565 conversion to `ffmpeg` and only relays finished frames, so it keeps up where a per-pixel Python loop cannot. It needs `ffmpeg` on `PATH`.

```bash
python3 tools/play_video.py /dev/ttyACM0 video.mp4 --loop
```

Any ffmpeg input works. A webcam:

```bash
python3 tools/play_video.py /dev/ttyACM0 /dev/video0 --format v4l2
```

The desktop:

```bash
python3 tools/play_video.py /dev/ttyACM0 :0.0 --format x11grab
```

Any resolution, frame rate, codec and container ffmpeg can open will play. The source rate is resampled to the send rate, so a 5 fps clip has frames duplicated and a 60 fps clip has them dropped, both at correct speed; variable-rate sources are normalised the same way. Anamorphic pixel aspect ratios are corrected rather than squashed.

`--fit` decides how a non-square source meets the square panel:

| | |
| --- | --- |
| `crop` (default) | Fill the panel, centre-crop the overflow. Loses the edges of a 16:9 source. |
| `pad` | Letterbox. Whole frame visible, black bars on the short axis. |
| `stretch` | Ignore aspect ratio. |

When the link falls behind, frames are skipped so playback tracks the wall clock instead of drifting; pass `--no-drop` to send every frame instead and let playback lag. On exit it prints the frame rate it actually achieved.

Audio is discarded. HDR and 10-bit sources play but are not tone-mapped, so they look washed out.

## Which port

On Linux the board is usually `/dev/ttyACM0`, but that number shifts as soon as another CDC device is plugged in. Prefer the stable path, which is tied to the descriptors in `usb_descriptors.c`:

```bash
/dev/serial/by-id/usb-e-lab_innovations_USB2TFT_000001-if00
```

`ls /dev/serial/by-id/` lists what is attached.

## Throughput

Measured end to end on an x86 Linux host:

| | |
| --- | --- |
| USB CDC bulk OUT | ~512 KiB/s |
| Frame size | 32,768 bytes |
| Sustained frame rate | **~16 fps** |

USB Full Speed is the limit, not the panel. SPI runs at 31.25 MHz, so DMA pushes a frame to the display in 8.4 ms — about 2% of the frame budget; with panel writes disabled entirely, throughput stays at ~512 KiB/s. Going faster therefore means sending fewer bytes (12-bit RGB444 would buy ~1.33×) or moving off CDC to a vendor bulk class, not optimising the display path.

## Current status

- ST7735S initialization: working
- SPI display writes: working, DMA at 31.25 MHz with double buffering
- USB CDC RGB565 framebuffer protocol: working
- Live video playback: working at ~16 fps
