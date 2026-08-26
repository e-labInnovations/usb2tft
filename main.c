
#include <string.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "tusb.h"
#include "bsp/board_api.h"

#include "audio.h"
#include "splash.h"

// --- Pin definitions ---
// GP3 dead, GP6 was bit-bang MOSI → now using hardware SPI0
// Move SDA wire from GP6 → GP7 (SPI0 TX)
#define PIN_MOSI  7   // SPI0 TX
#define PIN_SCK   2   // SPI0 SCK — no wire change
#define PIN_CS    5   // manual GPIO CS
#define PIN_DC    4   // RS on display
#define PIN_RST   0   // confirmed working

#define SPI_PORT  spi0

// The RP2040 divides its 125 MHz peripheral clock, so the reachable rates are
// 125/2, 125/4, 125/6 ... MHz.  31.25 MHz sends a full frame in 8.4 ms, which
// keeps the panel well ahead of the USB link.  Drop to 15625000 (125/8) if long
// or unshielded wiring produces speckled pixels.
#define SPI_HZ    31250000

// --- ST7735 commands ---
#define ST77XX_SWRESET  0x01
#define ST77XX_SLPOUT   0x11
#define ST77XX_NORON    0x13
#define ST77XX_INVOFF   0x20
#define ST77XX_INVON    0x21
#define ST77XX_DISPON   0x29
#define ST77XX_CASET    0x2A
#define ST77XX_RASET    0x2B
#define ST77XX_RAMWR    0x2C
#define ST77XX_MADCTL   0x36
#define ST77XX_COLMOD   0x3A
#define ST7735_FRMCTR1  0xB1
#define ST7735_FRMCTR2  0xB2
#define ST7735_FRMCTR3  0xB3
#define ST7735_INVCTR   0xB4
#define ST7735_PWCTR1   0xC0
#define ST7735_PWCTR2   0xC1
#define ST7735_PWCTR3   0xC2
#define ST7735_PWCTR4   0xC3
#define ST7735_PWCTR5   0xC4
#define ST7735_VMCTR1   0xC5
#define ST7735_GMCTRP1  0xE0
#define ST7735_GMCTRN1  0xE1

#define TFT_WIDTH   128
#define TFT_HEIGHT  128
#define FRAME_BYTES (TFT_WIDTH * TFT_HEIGHT * 2)

// The ST7735S has 132 x 162 of GRAM but this panel's glass only shows a
// 128 x 128 window inside it, starting at column 2 and row 1.  Without these
// offsets the last two columns and the last row of the visible area are never
// written and show whatever the controller powered up with (white, plus a few
// pixels that track our data).  Two different panels showed it identically,
// which is what ruled out the physical edge damage on the first one.
#define TFT_COL_OFFSET  2
#define TFT_ROW_OFFSET  1

// USB chunk protocol.  Every chunk is a 4-byte magic and a big-endian u32
// payload length, and the reader scans for the magic rather than assuming the
// stream is aligned, so a sender that dies mid-chunk costs one chunk instead of
// desynchronising everything after it.
//
//   "TFT1" 00 00 80 00  [32,768 RGB565 bytes, big-endian]   one full frame
//   "PCM1" 00 00 04 00  [1,024 bytes of s16le mono]         audio samples
//
// Replies, one byte each, so the host can pace itself and count what went
// wrong without a second endpoint:
//   'K'  a frame has finished painting
//   'O'  audio arrived faster than it could be played, and was dropped
//   'U'  audio ran dry and silence was played instead
//   'R'  the audio DMA chain had stopped and was restarted
//
// "STAT" with a zero length asks for counters: the reply is 'S' followed by
// four big-endian u32s, frames handed to I2S, underruns, overflows, restarts.
// Two of those a few seconds apart give the real sample clock.
#define MAGIC_LEN 4
static const char MAGIC_VIDEO[MAGIC_LEN] = { 'T', 'F', 'T', '1' };
static const char MAGIC_PCM[MAGIC_LEN]   = { 'P', 'C', 'M', '1' };
static const char MAGIC_STAT[MAGIC_LEN]  = { 'S', 'T', 'A', 'T' };

// A single audio chunk should be short enough that it never delays a frame for
// long: 1 KB is 23 ms of audio and 2 ms of bus time.
#define AUDIO_CHUNK_MAX 4096

typedef enum { RX_MAGIC, RX_LENGTH, RX_VIDEO, RX_AUDIO } rx_state_t;
typedef enum { CHUNK_VIDEO, CHUNK_PCM, CHUNK_STAT } chunk_kind_t;

#define FRAME_ACK    'K'
#define AUDIO_OVERFLOW_ACK 'O'
#define AUDIO_UNDERRUN_ACK 'U'
#define AUDIO_RESTART_ACK  'R'
#define STAT_REPLY         'S'

// 22.05 kHz mono, half of CD rate.  A one inch speaker does not reproduce the
// 11 kHz this leaves, and as IMA ADPCM it costs 2.1% of a bus that video has
// already filled.  See docs/audio-plan.md.
#define AUDIO_SAMPLE_RATE 22050

// Two framebuffers: USB fills one while DMA streams the other to the panel.
// The RP2040's default main stack is far smaller than a single framebuffer, so
// these must live in static RAM.
static uint8_t frame_buf[2][FRAME_BYTES];

static int dma_chan = -1;
static bool dma_busy = false;
static bool ack_pending = false;

// --- Hardware SPI ---
static inline void cs_low()  { gpio_put(PIN_CS, 0); }
static inline void cs_high() { gpio_put(PIN_CS, 1); }

static void spi_write8(uint8_t byte) {
    spi_write_blocking(SPI_PORT, &byte, 1);
}

static void spi_write16(uint16_t word) {
    uint8_t buf[2] = { word >> 8, word & 0xFF };
    spi_write_blocking(SPI_PORT, buf, 2);
}

static inline void st_cmd(uint8_t cmd) {
    gpio_put(PIN_DC, 0);
    spi_write8(cmd);
    gpio_put(PIN_DC, 1);
}

static inline void st_data8(uint8_t d)   { spi_write8(d); }
static inline void st_data16(uint16_t d) { spi_write16(d); }

// --- GPIO init ---
static void gpio_init_all(void) {
    spi_init(SPI_PORT, SPI_HZ);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);

    gpio_init(PIN_CS);  gpio_set_dir(PIN_CS,  GPIO_OUT); gpio_put(PIN_CS,  1);
    gpio_init(PIN_DC);  gpio_set_dir(PIN_DC,  GPIO_OUT); gpio_put(PIN_DC,  1);
    gpio_init(PIN_RST); gpio_set_dir(PIN_RST, GPIO_OUT); gpio_put(PIN_RST, 1);
}

static void set_full_window(void) {
    const uint8_t x0 = TFT_COL_OFFSET, x1 = TFT_COL_OFFSET + TFT_WIDTH  - 1;
    const uint8_t y0 = TFT_ROW_OFFSET, y1 = TFT_ROW_OFFSET + TFT_HEIGHT - 1;

    st_cmd(ST77XX_CASET);
    st_data8(0); st_data8(x0); st_data8(0); st_data8(x1);
    st_cmd(ST77XX_RASET);
    st_data8(0); st_data8(y0); st_data8(0); st_data8(y1);
    st_cmd(ST77XX_RAMWR);
}

// Paint the whole 132 x 162 GRAM, not just the visible window, so that any
// pixel outside the addressed area is black rather than power-on white.  Only
// needed once at boot; every frame after that goes through set_full_window().
#define TFT_GRAM_WIDTH   132
#define TFT_GRAM_HEIGHT  162

static void bare_fill_gram(uint16_t color) {
    cs_low();
    st_cmd(ST77XX_CASET);
    st_data8(0); st_data8(0); st_data8(0); st_data8(TFT_GRAM_WIDTH - 1);
    st_cmd(ST77XX_RASET);
    st_data8(0); st_data8(0); st_data8(0); st_data8(TFT_GRAM_HEIGHT - 1);
    st_cmd(ST77XX_RAMWR);
    for (uint32_t i = 0; i < TFT_GRAM_WIDTH * TFT_GRAM_HEIGHT; i++) st_data16(color);
    cs_high();
}

// Boot screen.  The panel holds whatever was last written to it, so this needs
// no timer: it stays up until the first USB frame overwrites it.  Painted
// blocking, before tusb_init(), because 32 KB at 31.25 MHz is 8.4 ms and there
// is no USB traffic to starve yet.
static void draw_splash(void) {
    _Static_assert(SPLASH_BYTES == FRAME_BYTES, "splash.h does not match the panel");
    cs_low();
    set_full_window();
    spi_write_blocking(SPI_PORT, splash_rgb565, SPLASH_BYTES);
    cs_high();
}

static void bare_fill(uint16_t color) {
    cs_low();
    set_full_window();
    for (uint32_t i = 0; i < TFT_WIDTH * TFT_HEIGHT; i++) st_data16(color);
    cs_high();
}

// Hand a completed framebuffer to DMA and return immediately.  CS stays
// asserted for the whole transfer and is released in display_poll().
static void display_start(const uint8_t *pixels) {
    cs_low();
    set_full_window();

    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, spi_get_dreq(SPI_PORT, true));

    dma_channel_configure(dma_chan, &cfg,
                          &spi_get_hw(SPI_PORT)->dr,  // write to the SPI data register
                          pixels,                     // read from the framebuffer
                          FRAME_BYTES,
                          true);                      // start now
    dma_busy = true;
}

static void display_poll(void) {
    if (!dma_busy || dma_channel_is_busy(dma_chan)) return;

    // DMA has queued the last byte; the SPI FIFO still has to shift it out
    // before CS may be released.
    while (spi_is_busy(SPI_PORT)) tight_loop_contents();
    cs_high();
    dma_busy = false;
    ack_pending = true;
}

// Audio faults are reported as one byte per event rather than as a counter the
// host has to ask for, so an underrun ends up in the sender's summary line
// instead of being something you hear and guess at.
static uint32_t reported_underruns = 0;
static uint32_t reported_overflows = 0;
static uint32_t reported_restarts = 0;
static bool stat_pending = false;

static void write_be32(uint32_t value) {
    tud_cdc_write_char((value >> 24) & 0xff);
    tud_cdc_write_char((value >> 16) & 0xff);
    tud_cdc_write_char((value >> 8) & 0xff);
    tud_cdc_write_char(value & 0xff);
}

static void service_ack(void) {
    if (tud_cdc_write_available() == 0) return;

    if (ack_pending) {
        tud_cdc_write_char(FRAME_ACK);
        ack_pending = false;
    }
    if (audio_underruns() != reported_underruns) {
        reported_underruns++;
        tud_cdc_write_char(AUDIO_UNDERRUN_ACK);
    }
    if (audio_overflows() != reported_overflows) {
        reported_overflows++;
        tud_cdc_write_char(AUDIO_OVERFLOW_ACK);
    }
    if (audio_stream_restarts() != reported_restarts) {
        reported_restarts++;
        tud_cdc_write_char(AUDIO_RESTART_ACK);
    }
    if (stat_pending && tud_cdc_write_available() >= 17) {
        tud_cdc_write_char(STAT_REPLY);
        write_be32(audio_samples_out());
        write_be32(audio_underruns());
        write_be32(audio_overflows());
        write_be32(audio_stream_restarts());
        stat_pending = false;
    }
    tud_cdc_write_flush();
}

// Opening the port at 1200 baud reboots into BOOTSEL, the same convention the
// Pico's stdio driver and Arduino use.  Reflashing then needs no button press.
#define RESET_MAGIC_BAUD 1200

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding) {
    (void) itf;
    if (coding->bit_rate == RESET_MAGIC_BAUD) reset_usb_boot(0, 0);
}

static void st7735_init(void) {
    // CS is active LOW on the ST7735S.  Keep it asserted for the complete
    // command sequence, then deassert it between transactions.
    cs_low();

    st_cmd(ST77XX_SWRESET); sleep_ms(150);
    st_cmd(ST77XX_SLPOUT);  sleep_ms(150);

    st_cmd(ST7735_FRMCTR1); st_data8(0x01); st_data8(0x2C); st_data8(0x2D);
    st_cmd(ST7735_FRMCTR2); st_data8(0x01); st_data8(0x2C); st_data8(0x2D);
    st_cmd(ST7735_FRMCTR3);
    st_data8(0x01); st_data8(0x2C); st_data8(0x2D);
    st_data8(0x01); st_data8(0x2C); st_data8(0x2D);
    st_cmd(ST7735_INVCTR);  st_data8(0x07);

    st_cmd(ST7735_PWCTR1); st_data8(0xA2); st_data8(0x02); st_data8(0x84);
    st_cmd(ST7735_PWCTR2); st_data8(0xC5);
    st_cmd(ST7735_PWCTR3); st_data8(0x0A); st_data8(0x00);
    st_cmd(ST7735_PWCTR4); st_data8(0x8A); st_data8(0x2A);
    st_cmd(ST7735_PWCTR5); st_data8(0x8A); st_data8(0xEE);
    st_cmd(ST7735_VMCTR1); st_data8(0x0E);

    // This panel's colour filter is physically B-G-R, so bit 3 of MADCTL is set
    // to make the controller swap red and blue for us and ordinary RGB565 comes
    // out correct.  With the bit clear, 0xF800 paints blue.  Note that the bit
    // never moves green, so a host-side (B, R, G) packing cannot be corrected
    // here at all -- see the FitPro-LT715-TLSR8232 notes on the same panel.
    // No rotation.  COLMOD 0x05 selects 16-bit RGB565 pixels.
    st_cmd(ST77XX_MADCTL); st_data8(0x08);
    st_cmd(ST77XX_COLMOD); st_data8(0x05);
    st_cmd(ST77XX_INVOFF);
    st_cmd(ST77XX_NORON);  sleep_ms(10);
    st_cmd(ST77XX_DISPON); sleep_ms(100);

    cs_high();
}

int main(void) {
    // Use TinyUSB's standard RP2040 board/device startup sequence.  No
    // stdio_init_all(): nothing here prints, and the default UART would
    // otherwise claim GP0, which drives the panel's reset line.
    board_init();
    gpio_init_all();

    gpio_put(PIN_RST, 0); sleep_ms(50);
    gpio_put(PIN_RST, 1); sleep_ms(150);

    st7735_init();
    bare_fill_gram(0x0000);
    draw_splash();

    dma_chan = dma_claim_unused_channel(true);

    audio_init(AUDIO_SAMPLE_RATE);
#if AUDIO_TEST_TONE
    // Milestone 1: prove the clock, the wiring, the amp and the speaker with
    // nothing else in the path.  USB audio does not exist yet.
    audio_set_source(audio_test_tone);

    // A receiver fed the wrong frame format buzzes rather than going silent, so
    // all four are swept, four seconds each, and the panel says which one is
    // live.  Same trick as painting the screen red and green during the USB
    // bring-up: there is no serial console to print to.
    static const uint16_t format_colour[AUDIO_FMT_COUNT] = {
        0xF800,  // AUDIO_FMT_I2S      red
        0x07E0,  // AUDIO_FMT_I2S_INV  green
        0x001F,  // AUDIO_FMT_LJ       blue
        0xFFFF,  // AUDIO_FMT_LJ_INV   white
    };
    uint32_t tone_format = 0;
    uint32_t tone_switch_us = time_us_32();
#endif

    tusb_rhport_init_t usb_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(0, &usb_init); // RP2040's built-in USB controller

    rx_state_t rx_state = RX_MAGIC;
    char magic_window[MAGIC_LEN] = { 0 };
    chunk_kind_t kind = CHUNK_VIDEO;
    uint32_t payload_len = 0;
    uint32_t payload_pos = 0;
    uint8_t length_bytes = 0;
    uint8_t fill_index = 0;
    uint8_t audio_scratch[256];

#if !AUDIO_TEST_TONE
    audio_set_source(audio_ring_source);
#endif

    while (true) {
        tud_task();
        display_poll();
        service_ack();
        audio_poll();

#if AUDIO_TEST_TONE
        if (time_us_32() - tone_switch_us >= 4u * 1000000u) {
            tone_switch_us = time_us_32();
            audio_set_format((audio_format_t) tone_format);
            bare_fill(format_colour[tone_format]);
            tone_format = (tone_format + 1) % AUDIO_FMT_COUNT;
        }
#endif

        while (tud_cdc_available()) {
            if (rx_state == RX_MAGIC) {
                // Slide a four byte window along the stream.  Scanning rather
                // than counting is what lets the reader recover on its own,
                // and it costs one byte-at-a-time read per header.
                uint8_t byte;
                tud_cdc_read(&byte, 1);
                magic_window[0] = magic_window[1];
                magic_window[1] = magic_window[2];
                magic_window[2] = magic_window[3];
                magic_window[3] = (char) byte;

                if (memcmp(magic_window, MAGIC_VIDEO, MAGIC_LEN) == 0) {
                    kind = CHUNK_VIDEO;
                } else if (memcmp(magic_window, MAGIC_PCM, MAGIC_LEN) == 0) {
                    kind = CHUNK_PCM;
                } else if (memcmp(magic_window, MAGIC_STAT, MAGIC_LEN) == 0) {
                    kind = CHUNK_STAT;
                } else {
                    continue;
                }
                rx_state = RX_LENGTH;
                payload_len = 0;
                length_bytes = 0;
                continue;
            }

            if (rx_state == RX_LENGTH) {
                uint8_t byte;
                tud_cdc_read(&byte, 1);
                payload_len = (payload_len << 8) | byte;
                if (++length_bytes < 4) continue;

                // A length the firmware cannot honour means the magic was a
                // coincidence in the middle of a payload, so go back to
                // scanning instead of trusting it.
                bool sane;
                switch (kind) {
                    case CHUNK_VIDEO: sane = (payload_len == FRAME_BYTES); break;
                    case CHUNK_PCM:   sane = (payload_len > 0 &&
                                              payload_len <= AUDIO_CHUNK_MAX); break;
                    default:          sane = (payload_len == 0); break;
                }
                if (!sane) {
                    rx_state = RX_MAGIC;
                    continue;
                }
                if (kind == CHUNK_STAT) {
                    stat_pending = true;
                    rx_state = RX_MAGIC;
                    continue;
                }
                rx_state = (kind == CHUNK_VIDEO) ? RX_VIDEO : RX_AUDIO;
                payload_pos = 0;
                continue;
            }

            if (rx_state == RX_VIDEO) {
                // Bulk-copy whatever has arrived straight into the framebuffer.
                uint32_t got = tud_cdc_read(&frame_buf[fill_index][payload_pos],
                                            payload_len - payload_pos);
                if (got == 0) break;
                payload_pos += got;
                if (payload_pos < payload_len) continue;

                // The previous frame is normally long gone (8.4 ms of DMA
                // against ~28 ms of USB), but never overwrite a buffer that
                // is still being shifted out.  Audio keeps running here: it is
                // fed by its own DMA and refilled from an interrupt.
                while (dma_busy) {
                    tud_task();
                    display_poll();
                }
                display_start(frame_buf[fill_index]);
                fill_index ^= 1;
                rx_state = RX_MAGIC;
                continue;
            }

            // RX_AUDIO.  Small reads, because these bytes are copied into the
            // ring rather than landing in their final place, and because a
            // long audio chunk must not hold up tud_task().
            uint32_t want = payload_len - payload_pos;
            if (want > sizeof(audio_scratch)) want = sizeof(audio_scratch);
            uint32_t got = tud_cdc_read(audio_scratch, want);
            if (got == 0) break;

            // Audio after a gap is a new stream, so its counters start clean
            // and the host is not handed the previous session's faults.
            if (audio_idle_us() > 1000000u) {
                audio_reset_stats();
                reported_underruns = 0;
                reported_overflows = 0;
            }
            payload_pos += got;
            audio_push(audio_scratch, got);
            if (payload_pos == payload_len) rx_state = RX_MAGIC;
        }
    }
}
