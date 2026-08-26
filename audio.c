#include <math.h>
#include <string.h>

#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "audio.h"
#include "i2s.pio.h"

// --- Pins ---
// GP8, GP9 and GP10 are free: the panel takes GP0, GP2, GP4, GP5 and GP7, and
// GP3 is dead on this board.  BCLK and LRCLK must stay consecutive because one
// side-set field drives both.
#define PIN_I2S_DIN   8
#define PIN_I2S_BCLK  9   // LRCLK is GP10

// One half-buffer is 256 stereo frames, 11.6 ms at 22.05 kHz.  Two of them is
// 2 KB, against 64 KB of framebuffers in a 264 KB part.  Small enough that a
// late refill is a click rather than a stall, big enough that the interrupt
// fires under 100 times a second.
#define HALF_FRAMES 256

// Stereo frames as packed words: left in the high 16 bits, because the PIO
// program shifts left and the MSB leaves first.
static uint32_t audio_buf[2][HALF_FRAMES];
static int16_t mono_scratch[HALF_FRAMES];

static PIO audio_pio = pio0;
static uint audio_sm;
static int audio_dma[2] = { -1, -1 };
static volatile audio_source_t audio_src = NULL;
static volatile uint32_t underruns = 0;
static uint32_t audio_fs = 0;

// SD_MODE on the MAX98357A selects which channel the single speaker gets, so
// the same mono sample goes into both halves of the frame and the wiring
// decides nothing.
static void fill_half(uint32_t *dst) {
    audio_source_t src = audio_src;
    size_t got = 0;

    if (src != NULL) got = src(mono_scratch, HALF_FRAMES);
    if (got < HALF_FRAMES) {
        if (src != NULL) underruns++;
        memset(&mono_scratch[got], 0, (HALF_FRAMES - got) * sizeof(mono_scratch[0]));
    }

    for (size_t i = 0; i < HALF_FRAMES; i++) {
        uint32_t sample = (uint16_t) mono_scratch[i];
        dst[i] = (sample << 16) | sample;
    }
}

// The two channels chain to each other, so the next transfer is already armed
// when one finishes and the I2S stream never gaps.  A chained channel restarts
// from whatever its registers hold, and DMA leaves the read pointer at the end
// of the buffer, so the finished channel is rewound here.  There is a full
// half-buffer of time to do it in.
static void __isr audio_dma_handler(void) {
    for (int i = 0; i < 2; i++) {
        if (!(dma_hw->ints1 & (1u << audio_dma[i]))) continue;
        dma_hw->ints1 = 1u << audio_dma[i];

        fill_half(audio_buf[i]);
        dma_channel_set_read_addr(audio_dma[i], audio_buf[i], false);
        dma_channel_set_trans_count(audio_dma[i], HALF_FRAMES, false);
    }
}

void audio_init(uint32_t sample_rate) {
    audio_fs = sample_rate;

    uint offset = pio_add_program(audio_pio, &i2s_out_program);
    audio_sm = pio_claim_unused_sm(audio_pio, true);
    i2s_out_program_init(audio_pio, audio_sm, offset,
                         PIN_I2S_DIN, PIN_I2S_BCLK, sample_rate);

    audio_dma[0] = dma_claim_unused_channel(true);
    audio_dma[1] = dma_claim_unused_channel(true);

    for (int i = 0; i < 2; i++) {
        memset(audio_buf[i], 0, sizeof(audio_buf[i]));

        dma_channel_config cfg = dma_channel_get_default_config(audio_dma[i]);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
        channel_config_set_read_increment(&cfg, true);
        channel_config_set_write_increment(&cfg, false);
        channel_config_set_dreq(&cfg, pio_get_dreq(audio_pio, audio_sm, true));
        channel_config_set_chain_to(&cfg, audio_dma[i ^ 1]);

        dma_channel_configure(audio_dma[i], &cfg,
                              &audio_pio->txf[audio_sm],
                              audio_buf[i],
                              HALF_FRAMES,
                              false);
        dma_channel_set_irq1_enabled(audio_dma[i], true);
    }

    // DMA_IRQ_1, because the panel's transfer is polled on IRQ 0's side of the
    // fence and audio must not wait behind a 32 KB frame.
    irq_set_exclusive_handler(DMA_IRQ_1, audio_dma_handler);
    irq_set_enabled(DMA_IRQ_1, true);

    dma_channel_start(audio_dma[0]);
}

void audio_set_source(audio_source_t source) {
    audio_src = source;
}

uint32_t audio_underruns(void) {
    return underruns;
}

// --- Test tone ---
// One cycle in a table plus a phase accumulator, so the tone frequency does
// not have to divide the sample rate.  440 Hz at 22.05 kHz is 50.1 samples per
// cycle, which a table read directly would detune audibly.
#define TONE_TABLE_LEN 256
#define TONE_HZ        440
#define TONE_AMPLITUDE 8000   // about a quarter scale, so the amp stays honest

static int16_t tone_table[TONE_TABLE_LEN];
static uint32_t tone_phase = 0;
static uint32_t tone_step = 0;

static void tone_init(uint32_t sample_rate) {
    for (int i = 0; i < TONE_TABLE_LEN; i++) {
        tone_table[i] = (int16_t) (TONE_AMPLITUDE *
                                   sinf(2.0f * (float) M_PI * i / TONE_TABLE_LEN));
    }
    // Phase in 16.16, wrapping over the table length.
    tone_step = (uint32_t) (((uint64_t) TONE_HZ * TONE_TABLE_LEN << 16) / sample_rate);
}

size_t audio_test_tone(int16_t *samples, size_t count) {
    if (tone_step == 0) tone_init(audio_fs);
    for (size_t i = 0; i < count; i++) {
        samples[i] = tone_table[(tone_phase >> 16) % TONE_TABLE_LEN];
        tone_phase += tone_step;
    }
    return count;
}
