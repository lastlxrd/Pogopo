# STEP9.2 — Game Boy audio/core restore

This patch ports the scheduling and audio rhythm from the last stable Arduino pogopoOS1.0 build into ESP-IDF.

## Stable core split restored

- Core 1 priority 7: TCA9555 input
- Core 1 priority 6: Peanut-GB CPU + MiniGB APU producer
- Core 0 priority 8: I2S audio output
- Core 0 priority 1: GUI + Sharp presentation

## Audio path

- Exact 32768 Hz source and hardware rate
- Exact 16724 us Game Boy frame pacing
- 8 x 512 I2S DMA configuration
- 4096-frame internal-RAM stereo ring
- Four silent packets prefilled at launch
- Game Boy realtime audio gets an exclusive mixer fast path
- No resampling at 32768 -> 32768
- Whole frame packets are accepted or dropped; partial packets are forbidden

## Emulator hot-path fixes

- Display publish divider removed (every emulated frame may be presented)
- Binary 1-bit shade conversion like pogopoOS1.0
- Render scratch buffer moved to PSRAM first
- Internal ROM allocation attempted up to 512 KiB
- PSRAM ROM cache defaults to 4 x 16 KiB in internal RAM
- Octal PSRAM speed raised from 40 MHz to 80 MHz for large cartridges
- Cache statistics no longer perform atomics on every ROM byte
- Automatic save writes during gameplay disabled; saves flush on exit/power-off

## Serial diagnostic

`PERF emu=... lcd=... frame=... ROM=INT/PSRAM cache=... audio=buffer/cap under=... over=... drop=...`
