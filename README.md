# STEP9.3 — Kirby stability / audio / input patch

This patch targets the exact failures seen in the STEP9.2 serial log.

## What the log proved

- Super Mario Land runs at 60–63 emulated FPS from internal RAM.
- Kirby Dream Land cannot fit its 256 KiB ROM in the available internal heap, so
  it uses Octal PSRAM and the internal bank cache.
- Kirby falls below the 16.724 ms frame budget, drains the fixed-rate 32768 Hz
  audio ring, and repeatedly underruns.
- A continuously late priority-6 emulator task never blocks, starving IDLE1 and
  triggering the task watchdog.
- An I2C read failure preserved the previous held button state.

## Changes

- Game Boy component builds with `-O3`.
- PSRAM cartridges request up to 8 x 16 KiB internal cache pages, with automatic
  fallback to 6/4/2 pages if allocation is tight.
- PSRAM cartridges automatically skip LCD rendering on alternate frames. CPU,
  timers and APU still emulate every frame; the display target becomes a stable
  30 FPS instead of an unstable 15–40 FPS.
- BMI270 task moved from Core 1 to Core 0, leaving Core 1 for TCA9555 + Game Boy.
- Late emulator frames yield one real RTOS tick every 16 frames, preventing
  `IDLE1` watchdog faults with negligible average overhead.
- TCA9555 read timeout reduced to 10 ms. Failed reads now feed a released sample
  through the existing debounce path, so a direction cannot remain stuck.
- Realtime audio follows a smoothed measured emulator rate. At full speed it is
  exactly 32768 -> 32768; when the emulator is slow, samples are gently stretched
  instead of draining the ring and cracking.
- Emergency audio underruns fade to zero over 64 samples rather than cutting
  abruptly.
- PERF log now includes `speed=...%` and cumulative `i2cerr=...`.

## Expected test result

For Kirby, target values are approximately:

`PERF emu=55..60 lcd=28..30 ... audio=>500/4096 under=stable speed=90..100% i2cerr=0`

The most important checks are:

1. `under=` stops climbing continuously.
2. No `task_wdt` / `IDLE1` messages.
3. Releasing a direction always stops Kirby, even if one TCA read fails.
4. `cache=` reports more than four pages in the ROM load line when memory allows.
