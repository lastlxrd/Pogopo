# STEP9.4 — Fixed-rate audio, packed dithering, and internal ROM arena

STEP9.4 removes the unsuccessful timing compensation introduced in STEP9.3 and
moves the 256 KiB Kirby fast path into deterministic internal RAM.

## Why STEP9.3 regressed

- Adaptive audio changed the PCM consumption rate. It could hide a draining
  ring buffer, but changing playback speed also changed pitch.
- Automatic Peanut-GB frame skip made PSRAM games present only every second
  emulated frame, so a 20 FPS game could become a 10 FPS picture.
- The local `-O3` override increased the interpreter's code footprint and could
  put more pressure on the ESP32-S3 instruction cache than `-O2`.
- An 8 x 16 KiB ROM cache consumed up to 128 KiB of internal RAM without moving
  the complete Kirby cartridge out of PSRAM.

## Fixed-rate Game Boy audio

- MiniGB APU and MAX98357A remain locked to `32768 -> 32768 Hz`.
- Runtime playback-rate compensation and the `speed=...%` diagnostic are gone.
- The normal path consumes exactly one stereo source frame per I2S frame.
- An underrun fades out and then fades back in over 64 samples; it never changes
  playback pitch to conceal slow emulation.
- The four-packet startup prefill and whole-packet ring writes remain.

## Packed four-shade framebuffer and restored dithering

All four original Game Boy shade indices are stored at 2 bits per pixel:

- one 160 x 144 frame: `23040 -> 5760 bytes`;
- three frame buffers: `69120 -> 17280 bytes`;
- total saving: `51840 bytes`.

The Sharp renderer unpacks those indices directly into its 1 bpp framebuffer
and applies the same 2 x 2 Bayer pattern used before STEP9.2. This restores the
visible grey-texture dithering without returning to byte-per-pixel buffers.

## Internal ROM arena

At Game Boy startup, before input/GUI background tasks fragment the heap, the
frontend reserves one contiguous 256 KiB internal-RAM arena. ROMs up to that
size load directly into the reservation and reuse it across game launches.
Kirby and smaller cartridges should therefore report `ROM=ARENA`.

If the reservation cannot be created, the firmware remains bootable and logs a
warning before falling back to the regular internal heap and then PSRAM.

## PSRAM bank-cache fallback

Cartridges larger than the arena still use Octal PSRAM at 80 MHz. Their internal
cache is capped at 4 x 16 KiB with 3/2/1-page allocation fallback. When at least
two pages fit, bank 0 remains pinned and the remaining pages use LRU replacement.
Automatic LCD frame skipping is disabled for every ROM location.

## Scheduling and input safeguards retained from STEP9.3

- BMI270 stays on Core 0, leaving Core 1 to input and Peanut-GB.
- A continuously late emulator yields one real RTOS tick every 16 late frames.
- TCA9555 reads retain the 10 ms timeout and released-sample fail-safe.
- Save writes remain disabled during gameplay and flush on exit or power-off.

## Serial diagnostics

The once-per-second line is now:

```text
PERF emu=... lcd=... frame=...us max=...us ROM=ARENA/INT/PSRAM arena=...
fb=17280 cache=hits/misses miss_us=... audio=buffer/cap under=... over=...
drop=... heap=free/largest i2cerr=...
```

## Acceptance test

Use `1X FAST` for the first performance pass.

1. Mario and Kirby should report `ROM=ARENA`, `fb=17280`, and no automatic
   frame skip in the boot log.
2. Audio pitch must stay constant. `under=` may expose a genuinely missed
   deadline, but no adaptive stretching should be audible.
3. Target Mario values: `emu=59..60`, `lcd=28..30` or better.
4. Target Kirby values: `emu>=55`, preferably `59..60`; `lcd=28..30` or better.
5. `under=` and `drop=` should stop climbing after startup.
6. No `task_wdt` / `IDLE1` fault and no stuck direction after an I2C error.
7. Mid-tone Game Boy graphics must show the restored ordered-dither texture.

