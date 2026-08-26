#pragma once

#include <stdint.h>
#include <stddef.h>

// Mono sample source, called from the DMA interrupt to refill one half of the
// I2S buffer.  Write up to `count` samples and return how many were written;
// anything short is padded with silence and counted as an underrun.  Silence
// rather than a repeat of the last block, because a repeated block buzzes and
// a gap only clicks.
typedef size_t (*audio_source_t)(int16_t *samples, size_t count);

// Brings up the PIO state machine and the ping-pong DMA.  Starts silent.
void audio_init(uint32_t sample_rate);

// Installs the sample source.  Pass NULL for silence.
void audio_set_source(audio_source_t source);

// Number of half-buffers that could not be filled completely.  This is the
// number the host-side tools should print, so underruns are a measurement
// rather than something you hear and guess at.
uint32_t audio_underruns(void);

// 440 Hz sine from a table, for proving the wiring before any USB audio
// exists.  Install it with audio_set_source().
size_t audio_test_tone(int16_t *samples, size_t count);
