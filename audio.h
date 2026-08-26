#pragma once

#include <stdint.h>
#include <stddef.h>

// Mono sample source, called from the DMA interrupt to refill one half of the
// I2S buffer.  Write up to `count` samples and return how many were written;
// anything short is padded with silence and counted as an underrun.  Silence
// rather than a repeat of the last block, because a repeated block buzzes and
// a gap only clicks.
typedef size_t (*audio_source_t)(int16_t *samples, size_t count);

// Which frame format goes on the wire.  A receiver fed the wrong one of these
// does not go silent, it shifts every sample by a bit and buzzes, so the four
// are worth being able to switch between at runtime rather than at build time.
typedef enum {
    AUDIO_FMT_I2S = 0,      // LRCLK one bit early, data sampled on BCLK rising
    AUDIO_FMT_I2S_INV,      // same, BCLK inverted
    AUDIO_FMT_LJ,           // left justified, LRCLK in step with the first bit
    AUDIO_FMT_LJ_INV,       // same, BCLK inverted
    AUDIO_FMT_COUNT,
} audio_format_t;

// Brings up the PIO state machine and the ping-pong DMA.  Starts silent, in
// AUDIO_FMT_I2S.
void audio_init(uint32_t sample_rate);

// Reconfigures the state machine in place.  The DMA keeps running: it stalls on
// the state machine's DREQ while the format changes and picks up where it left
// off, so a switch costs a click and not a restart.
void audio_set_format(audio_format_t format);

// Installs the sample source.  Pass NULL for silence.
void audio_set_source(audio_source_t source);

// Number of half-buffers that could not be filled completely.  This is the
// number the host-side tools should print, so underruns are a measurement
// rather than something you hear and guess at.
uint32_t audio_underruns(void);

// Restarts the transfer chain if it has stopped.  Call from the main loop.
void audio_poll(void);

// How many times audio_poll() found the stream dead.  Anything but zero means
// something is stopping the DMA pair, not that the audio source ran out.
uint32_t audio_stream_restarts(void);

// --- USB audio ---
// Push s16le mono bytes into the ring.  Returns how many were accepted; short
// means the ring was full and the rest was dropped, which the host should see
// as a signal to slow down rather than as a fatal error.
size_t audio_push(const uint8_t *bytes, size_t count);

// Drops everything buffered.  For a seek or a new stream, where playing out
// the old audio first would be worse than a gap.
void audio_reset(void);

// How much audio is buffered, in milliseconds.  This is the number that tells
// the host whether it is ahead or behind.
uint32_t audio_buffered_ms(void);

// Times a push did not fit.  Overflow means the host is too far ahead;
// underrun means it is too far behind.
uint32_t audio_overflows(void);

// Clears the fault counters, and how long since audio last arrived, so a new
// stream can start from zero instead of inheriting an idle board's silence.
void audio_reset_stats(void);
uint32_t audio_idle_us(void);

// The ring, as a sample source for audio_set_source().
size_t audio_ring_source(int16_t *samples, size_t count);

// Frames handed to the I2S engine since boot, silence included.  Divided by
// elapsed time this is the real sample clock, which is the only way to tell a
// correct divider from a plausible one: both sound like audio, one sounds
// wrong.
uint32_t audio_samples_out(void);

// 440 Hz sine from a table, for proving the wiring before any USB audio
// exists.  Install it with audio_set_source().
size_t audio_test_tone(int16_t *samples, size_t count);
