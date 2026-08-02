# USB2TFT

Firmware for an RP2040 Zero driving a salvaged 128 × 128 ST7735S RGB565 display over SPI. The current firmware is a hardware bring-up test: it cycles the panel through red, green, and blue once per second.

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

1. Hold the RP2040 Zero's **BOOTSEL** button while connecting USB.
2. It appears as a removable drive named `RPI-RP2`.
3. Copy `build/usb2tft.uf2` to that drive.
4. The board restarts automatically.

With the current test firmware, the display should repeatedly show solid red, green, and blue screens. This confirms the SPI connection and ST7735S initialization before adding the USB framebuffer protocol.

## Current status

- ST7735S initialization: working
- SPI display writes: working
- RGB565 colour test: working
- USB framebuffer protocol: not yet implemented
