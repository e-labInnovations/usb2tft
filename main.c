
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "tusb.h"
#include "bsp/board_api.h"

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

// USB frame protocol:
//   54 46 54 31  00 00 80 00  [32,768 RGB565 bytes, big-endian]
//       "TFT1"       payload length
// One 'K' byte is sent back per displayed frame so the host can pace itself.
static const uint8_t frame_header[] = {
    'T', 'F', 'T', '1',
    (FRAME_BYTES >> 24) & 0xff,
    (FRAME_BYTES >> 16) & 0xff,
    (FRAME_BYTES >> 8) & 0xff,
    FRAME_BYTES & 0xff,
};

#define FRAME_ACK 'K'

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
    st_cmd(ST77XX_CASET);
    st_data8(0); st_data8(0); st_data8(0); st_data8(TFT_WIDTH - 1);
    st_cmd(ST77XX_RASET);
    st_data8(0); st_data8(0); st_data8(0); st_data8(TFT_HEIGHT - 1);
    st_cmd(ST77XX_RAMWR);
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

static void service_ack(void) {
    if (!ack_pending || tud_cdc_write_available() == 0) return;
    tud_cdc_write_char(FRAME_ACK);
    tud_cdc_write_flush();
    ack_pending = false;
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
    bare_fill(0x0000);

    dma_chan = dma_claim_unused_channel(true);

    tusb_rhport_init_t usb_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(0, &usb_init); // RP2040's built-in USB controller

    uint8_t header_matched = 0;
    uint32_t frame_pos = 0;
    uint8_t fill_index = 0;

    while (true) {
        tud_task();
        display_poll();
        service_ack();

        while (tud_cdc_available()) {
            if (header_matched < sizeof(frame_header)) {
                uint8_t byte;
                tud_cdc_read(&byte, 1);
                if (byte == frame_header[header_matched]) {
                    header_matched++;
                } else {
                    // The first header byte can also begin a new header.
                    header_matched = (byte == frame_header[0]) ? 1 : 0;
                }
                continue;
            }

            // Bulk-copy whatever has arrived straight into the framebuffer.
            uint32_t got = tud_cdc_read(&frame_buf[fill_index][frame_pos],
                                       FRAME_BYTES - frame_pos);
            if (got == 0) break;
            frame_pos += got;

            if (frame_pos == FRAME_BYTES) {
                // The previous frame is normally long gone (8.4 ms of DMA
                // against ~28 ms of USB), but never overwrite a buffer that
                // is still being shifted out.
                while (dma_busy) {
                    tud_task();
                    display_poll();
                }
                display_start(frame_buf[fill_index]);
                fill_index ^= 1;
                frame_pos = 0;
                header_matched = 0;
            }
        }
    }
}
