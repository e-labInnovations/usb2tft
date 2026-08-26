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
static uint audio_offset[2];        // [0] I2S, [1] left justified
static int audio_dma[2] = { -1, -1 };
static volatile audio_source_t audio_src = NULL;
static volatile uint32_t underruns = 0;
static uint32_t audio_fs = 0;
static volatile uint32_t stream_restarts = 0;

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

// --- Stream control ---
// The two channels chain to each other, which is what keeps the I2S stream
// gapless, and is also what makes stopping them awkward: abort one and the
// chain starts the other, and if both end up idle the stream is dead with
// nothing left running to restart it.  So chaining is pointed at each channel
// itself first, which is how the hardware spells "no chain", and only then are
// they aborted.
static void stream_configure(int i, bool chained) {
    dma_channel_config cfg = dma_channel_get_default_config(audio_dma[i]);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, pio_get_dreq(audio_pio, audio_sm, true));
    channel_config_set_chain_to(&cfg, chained ? audio_dma[i ^ 1] : audio_dma[i]);

    dma_channel_configure(audio_dma[i], &cfg,
                          &audio_pio->txf[audio_sm],
                          audio_buf[i],
                          HALF_FRAMES,
                          false);
}

static void stream_stop(void) {
    for (int i = 0; i < 2; i++) stream_configure(i, false);
    for (int i = 0; i < 2; i++) dma_channel_abort(audio_dma[i]);
    // Drop the completion flags the aborts raised, so the handler does not
    // rewind a channel that is about to be reconfigured anyway.
    for (int i = 0; i < 2; i++) dma_hw->ints1 = 1u << audio_dma[i];
}

static void stream_start(void) {
    for (int i = 0; i < 2; i++) stream_configure(i, true);
    dma_channel_start(audio_dma[0]);
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

    // Both programs are loaded up front; together they are 16 of the PIO's 32
    // instruction slots, and switching format then costs no reload.
    audio_offset[0] = pio_add_program(audio_pio, &i2s_out_program);
    audio_offset[1] = pio_add_program(audio_pio, &lj_out_program);
    audio_sm = pio_claim_unused_sm(audio_pio, true);
    i2s_out_program_init(audio_pio, audio_sm, audio_offset[0],
                         PIN_I2S_DIN, PIN_I2S_BCLK, sample_rate, false);

    audio_dma[0] = dma_claim_unused_channel(true);
    audio_dma[1] = dma_claim_unused_channel(true);
    for (int i = 0; i < 2; i++) {
        memset(audio_buf[i], 0, sizeof(audio_buf[i]));
        dma_channel_set_irq1_enabled(audio_dma[i], true);
    }

    // DMA_IRQ_1, because the panel's transfer is polled rather than interrupt
    // driven and audio must never wait behind a 32 KB frame.
    irq_set_exclusive_handler(DMA_IRQ_1, audio_dma_handler);
    irq_set_enabled(DMA_IRQ_1, true);

    stream_start();
}

void audio_set_format(audio_format_t format) {
    if (format >= AUDIO_FMT_COUNT) return;
    bool left_justified = (format == AUDIO_FMT_LJ || format == AUDIO_FMT_LJ_INV);
    bool invert_bclk = (format == AUDIO_FMT_I2S_INV || format == AUDIO_FMT_LJ_INV);

    stream_stop();
    pio_sm_set_enabled(audio_pio, audio_sm, false);
    pio_sm_clear_fifos(audio_pio, audio_sm);

    // Inverting BCLK in the pad rather than in the program keeps one copy of
    // each program and moves the sampling edge for both.
    gpio_set_outover(PIN_I2S_BCLK,
                     invert_bclk ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);

    i2s_out_program_init(audio_pio, audio_sm, audio_offset[left_justified ? 1 : 0],
                         PIN_I2S_DIN, PIN_I2S_BCLK, audio_fs, left_justified);
    stream_start();
}

// A dead chain is silence with no error, and silence is exactly what a wrong
// register write looks like from the outside.  If both channels are idle the
// stream has stopped, so restart it and count that, rather than leaving the
// speaker quiet and the cause invisible.
void audio_poll(void) {
    static uint32_t idle_since_us = 0;

    if (dma_channel_is_busy(audio_dma[0]) || dma_channel_is_busy(audio_dma[1])) {
        idle_since_us = 0;
        return;
    }

    // One channel finishing and chaining to the other leaves a window where
    // neither reads as busy, so a single idle sample proves nothing.  A real
    // stall outlasts a half buffer; 5 ms is comfortably inside that and well
    // past the handoff.
    uint32_t now = time_us_32();
    if (idle_since_us == 0) {
        idle_since_us = now ? now : 1;
        return;
    }
    if (now - idle_since_us < 5000) return;

    idle_since_us = 0;
    stream_restarts++;
    stream_start();
}

void audio_set_source(audio_source_t source) {
    audio_src = source;
}

uint32_t audio_underruns(void) {
    return underruns;
}

uint32_t audio_stream_restarts(void) {
    return stream_restarts;
}


// --- Sample ring ---
// Filled by the USB loop, drained by the DMA interrupt: one producer, one
// consumer, so a pair of indices and a power-of-two mask is the whole thing,
// no locking.  32-bit aligned loads and stores are atomic on this core, which
// is what makes that safe.
//
// 16 KB is 372 ms at 22.05 kHz mono.  Deep enough to ride out USB jitter and a
// host that pauses to decode, shallow enough that the lip sync correction the
// host has to apply stays small.
#define RING_BYTES 16384u
#define RING_MASK  (RING_BYTES - 1u)

static uint8_t ring[RING_BYTES];
static volatile uint32_t ring_head = 0;   // written by the producer
static volatile uint32_t ring_tail = 0;   // written by the consumer
static volatile uint32_t ring_overflows = 0;

static uint32_t ring_used(void) {
    return ring_head - ring_tail;
}

void audio_reset(void) {
    // Consumer side first: the interrupt reads tail, so moving head to meet it
    // cannot make the interrupt read a byte that is being overwritten.
    ring_tail = ring_head;
}

size_t audio_push(const uint8_t *bytes, size_t count) {
    uint32_t free_bytes = RING_BYTES - ring_used();
    if (count > free_bytes) {
        ring_overflows++;
        count = free_bytes;
    }
    for (size_t i = 0; i < count; i++) {
        ring[(ring_head + i) & RING_MASK] = bytes[i];
    }
    ring_head += count;
    return count;
}

uint32_t audio_buffered_ms(void) {
    // Two bytes per sample.
    return (ring_used() / 2) * 1000u / (audio_fs ? audio_fs : 1u);
}

uint32_t audio_overflows(void) {
    return ring_overflows;
}

// s16le on the wire, little endian in memory, so this could be a memcpy for
// most of its length.  It is written out byte by byte because a chunk boundary
// can land between the two halves of a sample and the ring wraps mid-sample.
size_t audio_ring_source(int16_t *samples, size_t count) {
    uint32_t available = ring_used() / 2;
    if (available < count) count = available;

    for (size_t i = 0; i < count; i++) {
        uint8_t lo = ring[(ring_tail + 0) & RING_MASK];
        uint8_t hi = ring[(ring_tail + 1) & RING_MASK];
        ring_tail += 2;
        samples[i] = (int16_t) ((uint16_t) lo | ((uint16_t) hi << 8));
    }
    return count;
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
