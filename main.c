#include "pico/stdlib.h"
#include "hardware/spi.h"

// --- Pin definitions ---
// GP3 dead, GP6 was bit-bang MOSI → now using hardware SPI0
// Move SDA wire from GP6 → GP7 (SPI0 TX)
#define PIN_MOSI  7   // SPI0 TX
#define PIN_SCK   2   // SPI0 SCK — no wire change
#define PIN_CS    5   // manual GPIO CS
#define PIN_DC    4   // RS on display
#define PIN_RST   0   // confirmed working

#define SPI_PORT  spi0

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
    // ST7735S accepts a much faster clock, but keep this conservative for
    // breadboard/flex-cable wiring while bringing the panel up.
    spi_init(SPI_PORT, 8000000);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);

    gpio_init(PIN_CS);  gpio_set_dir(PIN_CS,  GPIO_OUT); gpio_put(PIN_CS,  1);
    gpio_init(PIN_DC);  gpio_set_dir(PIN_DC,  GPIO_OUT); gpio_put(PIN_DC,  1);
    gpio_init(PIN_RST); gpio_set_dir(PIN_RST, GPIO_OUT); gpio_put(PIN_RST, 1);
}

static void bare_fill(uint16_t color) {
    cs_low();
    st_cmd(ST77XX_CASET);
    st_data8(0); st_data8(0); st_data8(0); st_data8(127);
    st_cmd(ST77XX_RASET);
    st_data8(0); st_data8(0); st_data8(0); st_data8(127);
    st_cmd(ST77XX_RAMWR);
    for (uint32_t i = 0; i < TFT_WIDTH * TFT_HEIGHT; i++) st_data16(color);
    cs_high();
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

    // RGB colour order, no rotation.  0x05 selects 16-bit RGB565 pixels.
    st_cmd(ST77XX_MADCTL); st_data8(0x00);
    st_cmd(ST77XX_COLMOD); st_data8(0x05);
    st_cmd(ST77XX_INVOFF);
    st_cmd(ST77XX_NORON);  sleep_ms(10);
    st_cmd(ST77XX_DISPON); sleep_ms(100);

    cs_high();
}

int main(void) {
    stdio_init_all();
    gpio_init_all();

    gpio_put(PIN_RST, 0); sleep_ms(50);
    gpio_put(PIN_RST, 1); sleep_ms(150);

    st7735_init();

    while (true) {
        bare_fill(0xF800); sleep_ms(1000); // red
        bare_fill(0x07E0); sleep_ms(1000); // green
        bare_fill(0x001F); sleep_ms(1000); // blue
    }
}
