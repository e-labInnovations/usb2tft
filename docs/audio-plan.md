# Adding sound: MAX98357A over PIO I2S

Design notes before any firmware exists. The point of writing it down first is
that three of the decisions here are hard to reverse once the wire protocol
ships, and one of them (power) can damage nothing but will make the panel
misbehave in a way that looks like a firmware bug.

Branch: `audio`. Nothing here is on `main` yet.

## What decides the design: the bus is already full

The measured ceiling on this board is 512 KiB/s, and video at 16 fps is
524,288 B/s. There is no spare bandwidth. Every byte of audio is a byte the
video does not get, so the format choice is a frame rate choice.

| Format, mono | Bytes/s | Share of bus | Video drops to |
| --- | --- | --- | --- |
| s16 PCM 22.05 kHz | 44,100 | 8.4% | ~14.6 fps |
| s16 PCM 16 kHz | 32,000 | 6.1% | ~15.0 fps |
| IMA ADPCM 22.05 kHz | 11,166 | 2.1% | ~15.7 fps |

Decision: **IMA ADPCM 4-bit, 22.05 kHz, mono**, with uncompressed PCM kept as a
second wire format. ADPCM is 4:1 with a fixed 4-bit-per-sample block layout,
decodes in a handful of cycles per sample, and ffmpeg already encodes it
(`-acodec adpcm_ima_wav`). Losing 0.3 fps to sound is a trade worth taking;
losing 1.4 fps is not, when the difference is a decoder that fits on one screen.

PCM stays because the first bring-up needs a path with no decoder in it. A
wrong ADPCM decoder and a wrong I2S clock both sound like noise, and debugging
two unknowns at once is how the colour bug survived a month.

22.05 kHz gives 11 kHz of audio bandwidth, which is already more than a one
inch speaker reproduces. The PIO clock divider lands on 44.28 rather than an
integer, a 0.05% pitch error, about 0.9 cents. Inaudible.

## Why PIO and not a peripheral

The RP2040 has no I2S peripheral. What people call "I2S on the RP2040" is
always PIO: a state machine shifts the data line and side-sets BCLK and LRCLK,
so the timing is in silicon and the CPU only refills a DMA buffer. Same
boundary as the USB argument earlier in this project.

`pico_audio_i2s` from pico-extras does this already, but pico-extras is not
installed here and it brings an audio buffer-pool abstraction, a mixer, and a
sample-rate converter for a job that is one PIO program plus one DMA channel.
Writing it directly keeps the dependency list at the SDK, the way the rest of
this firmware works.

Both PIO blocks are free: the panel is on hardware SPI0.

### Clocking

Standard I2S, 16 bits per channel, two channels per frame, so BCLK is 64x Fs
and the PIO program needs 2 cycles per bit:

    clkdiv = 125 MHz / (22050 x 64 x 2) = 44.28

The RP2040's divider has an 8-bit fraction, so 44.28 is reachable. The mono
stream is written into both channels, which costs I2S bit rate (free, it is a
local wire) rather than USB bandwidth (not free).

The MAX98357A needs no MCLK, which is the reason to prefer it here over an
I2S DAC that does.

## Wiring

| MAX98357A | RP2040-Zero | Note |
| --- | --- | --- |
| DIN | GP8 | I2S data, PIO out |
| BCLK | GP9 | PIO side-set, must be consecutive with LRCLK |
| LRCLK | GP10 | PIO side-set |
| VIN | 5V (VBUS) | 220 to 470 uF bulk cap right at the amp |
| GND | GND | Star back to the board's ground, not daisy chained through the panel |
| SD_MODE | 3V3 through 100k | Left channel; floating is shutdown on some modules |
| GAIN | leave floating | 9 dB, the sane default |

GP8, GP9 and GP10 are free: the panel uses GP0, GP2, GP4, GP5 and GP7, and GP3
is dead on this board.

The amp is rated 3.2 W into 4 ohms, which a 500 mA USB port cannot deliver. The
bulk cap is what keeps a bass transient from dragging VBUS down far enough to
brown out the RP2040, and a brownout mid-frame looks exactly like a firmware
fault. An 8 ohm speaker at moderate gain is the safe combination. If it ever
needs to be loud, the amp gets its own 5 V supply with grounds tied.

## Wire protocol

The current header is `"TFT1"` plus a big-endian u32 length, and the reader
scans for it rather than assuming alignment, so a sender that dies mid-frame
costs one frame instead of desynchronising the stream. That generalises without
breaking anything: keep the 8-byte shape, add magics.

| Magic | Payload | Meaning |
| --- | --- | --- |
| `TFT1` | 32,768 B | one full RGB565 frame, unchanged |
| `PCM1` | n x 2 B | s16le mono samples, 22.05 kHz |
| `ADP1` | ADPCM block | IMA ADPCM, `adpcm_ima_wav` block layout |
| `CFG1` | 8 B | sample rate, format, flags; sent once at start |

The scanner becomes a 4-byte compare against a small table instead of a single
byte-by-byte match. `TFT1` keeps its meaning and its length, so `send_frame.py`
and `color_test.py` need no changes at all.

Audio chunks want to be small, around 512 to 1024 bytes, so a chunk never
blocks a frame for long. At 11 KB/s that is a chunk every 45 to 90 ms.

## Buffering and who is the clock

Video can drop a frame and nobody notices. Audio cannot: a starved I2S DMA
produces a click on every underrun, which is the most audible failure mode
available. So audio gets priority in the firmware and the host has to keep the
device's ring ahead of real time.

- Device keeps a ring buffer of decoded s16 samples. 250 ms at 22.05 kHz is
  11 KB, which is nothing next to the two 32 KB framebuffers in a 264 KB part.
- I2S runs on two DMA half-buffers, refilled from the ring on DMA completion.
  If the ring is empty the refill writes silence rather than repeating the last
  block, because repeated blocks buzz and silence just gaps.
- The host paces on the existing frame ack. One extra ack byte, `A`, sent per
  consumed audio chunk, lets the sender track the ring level without a new
  endpoint or a status poll.
- On underrun the device sends `U` once per event. That makes underruns a
  measured number in the sender's summary line instead of something you hear
  and guess at.

Latency is set by the ring depth: 250 ms of audio means audio is 250 ms behind
the video unless the host delays the video by the same amount. The sender
already knows both streams' timestamps because ffmpeg gives them, so the
delay belongs on the host side, not in the firmware.

## Host side

`play_video.py` currently plays the source's audio on the computer with
ffplay, which was always a placeholder. The real version runs one ffmpeg with
two outputs, video as `rgb565be` and audio as `adpcm_ima_wav`, and interleaves
chunks by timestamp.

    ffmpeg -i in.mp4 \
      -map 0:v -vf "fps=16,scale=...,crop=128:128,setsar=1" -f rawvideo -pix_fmt rgb565be pipe:3 \
      -map 0:a -ac 1 -ar 22050 -acodec adpcm_ima_wav -f wav pipe:4

Reading two pipes means either two threads or a select loop; the select loop is
simpler and this sender is already single threaded.

`tools/play_audio.py` comes first though: audio only, no video, so the audio
path can be measured on its own.

## Milestones

Each one ends with something measurable, and none of them depends on the next
one working.

1. **Tone from flash.** PIO I2S plus DMA, a table sine at 440 Hz, no USB
    involvement. Proves clock, wiring, amp and speaker. If this buzzes, nothing
    downstream is worth debugging.
2. **PCM over USB.** `PCM1` chunks, ring buffer, underrun counter,
    `tools/play_audio.py` sending a WAV through ffmpeg. Audio only. Measures the
    real bandwidth cost and the ring depth needed to survive USB jitter.
3. **ADPCM.** `ADP1` and the decoder, same tool with a flag. Compare against
    step 2 on the same file, listening for decoder faults with a path known to
    work already available for A/B.
4. **Interleave with video.** `play_video.py` sends both. Measure the fps cost
    against the 15.7 fps baseline and count underruns over a full clip.
5. **Sync.** Delay the video by the ring depth on the host, check lip sync on
    something with speech in it.

## Open questions

- Does the ADPCM decode plus the USB reads plus the panel DMA still fit in one
  core's duty cycle at 16 fps? It should: the frame path is DMA and the decode
  is a few cycles per sample. If it does not, audio moves to core1, which is
  sitting idle. Not worth doing pre-emptively.
- The two alternating bulk OUT endpoints idea from the throughput work would
  roughly double the bus and make the whole format argument moot. Still
  unmeasured, still a USB layer rewrite.
- Volume control has no home yet. It could be a `CFG1` field applied as a
  shift on the decoded samples, which is cheap and coarse, or left to the host
  scaling the samples before encoding, which is free on the device.
