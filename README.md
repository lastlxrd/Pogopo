# STEP9.5.2 — Complete ASCII font coverage

This micro-fix builds directly on STEP9.5.1 and leaves the stable Game Boy,
audio, power-menu and 30 FPS LCD paths unchanged. The 5 x 7 system font now
covers every printable ASCII character from `0x20` through `0x7E`.

Game Boy filenames such as `Kirby's Dream Land`, `Tetris (World)` and ROM-set
names containing commas, brackets, ampersands or other ordinary punctuation no
longer render those characters as unknown-symbol boxes. The same coverage is
available to every OS screen and future built-in game that uses `font5x7()`.

## STEP9.5.1 — Power quick menu and UI polish

This micro-fix is based byte-for-byte on the stable STEP9.5 merge and leaves its
full-speed Game Boy / 30 FPS LCD pipeline unchanged. It adds:

- a short Power-button press that opens the quick menu;
- Resume, 5% volume adjustment, Settings and Home actions;
- Game Boy suspend/resume while the overlay owns input;
- the existing 650 ms `START + B` fast exit remains available;
- a physical Sharp Memory LCD clear immediately before BQ24295 ship mode;
- a quieter 1.15 kHz triangle-wave UI tick with a soft attack/release;
- app capacity raised from 12 to 24 so new built-in games are not silently
  rejected after the 12 existing OS apps are registered.

The Power task ignores the release used to boot the console, so the quick menu
cannot open by itself during startup. A two-second hold retains the existing
shutdown behavior.

## STEP9.5 performance baseline

The stable base ports the performance-critical choices from the last Arduino
firmware: 14 MHz Sharp LCD transfers, Core-0 decoupled APU generation, an
adaptive internal ROM cache, hot display buffers in internal RAM, full-speed
59.7 Hz Game Boy CPU emulation and approximately 29.9 Hz LCD presentation.

## Why STEP9.3 regressed

- Adaptive audio changed the PCM consumption rate. It could hide a draining
  ring buffer, but changing playback speed also changed pitch.
- Automatic Peanut-GB frame skip made PSRAM games present only every second
  emulated frame, so a 20 FPS game could become a 10 FPS picture.
- The local `-O3` override increased the interpreter's code footprint and could
  put more pressure on the ESP32-S3 instruction cache than `-O2`.
- An 8 x 16 KiB ROM cache consumed up to 128 KiB of internal RAM without moving
  the complete Kirby cartridge out of PSRAM.

## What the Arduino comparison found

- The ESP-IDF Sharp driver was configured for 2 MHz while the stable Arduino
  firmware used 14 MHz. A full 400 x 240 transfer at 2 MHz takes roughly 49 ms,
  so smooth Game Boy presentation was impossible even with a fast emulator.
- STEP9.4 generated one APU packet after each emulated frame. A slow frame
  therefore starved audio immediately. Arduino generated APU audio in a
  dedicated high-priority Core-0 task paced by blocking I2S output.
- The 256 KiB arena helped ROMs that fit, but was left allocated and unused for
  larger PSRAM ROMs. Those games could then fail to allocate a useful cache.
- Peanut-GB shade indices are `0=black, 1=dark, 2=light, 3=white`. STEP9.4 fed
  them to a renderer whose convention is the reverse, so the ordered dither
  was inverted compared with STEP9/9.1.

## Fixed-rate, frame-independent Game Boy audio

- MiniGB APU and MAX98357A remain locked to `32768 -> 32768 Hz`.
- Runtime playback-rate compensation and the `speed=...%` diagnostic are gone.
- MiniGB APU generation now runs in its own priority-7 task on Core 0. The
  priority-8 I2S writer consumes at the hardware sample clock and paces APU
  production through a four-packet watermark.
- The emulator no longer renders 548 stereo frames inside every video frame,
  freeing Core 1 and preventing slow video frames from directly draining audio.
- A dedicated APU mutex preserves the same cross-core protection as the stable
  Arduino implementation.
- An underrun fades out and then fades back in over 64 samples; it never changes
  playback pitch to conceal slow emulation.
- Ring writes copy at most two contiguous spans instead of doing one modulo
  operation for every stereo frame.

## Packed four-shade framebuffer and restored dithering

All four original Game Boy shade indices are stored at 2 bits per pixel:

- one 160 x 144 frame: `23040 -> 5760 bytes`;
- three frame buffers: `69120 -> 17280 bytes`;
- total saving: `51840 bytes`.

The Sharp renderer unpacks those indices directly into its 1 bpp framebuffer,
correctly reverses Peanut's black-to-white index order, and applies the same
2 x 2 ordered pattern used before STEP9.2. All three packed Game Boy buffers and
the 12 KiB Sharp framebuffer prefer internal RAM.

## 14 MHz Sharp LCD fast path

- SPI is restored from 2 MHz to the exact 14 MHz value used by the attached
  Arduino/Adafruit_SharpMem firmware on this board.
- A worst-case full-screen payload drops from about 49 ms on the wire to about
  7 ms. Dirty-row transfers remain enabled.
- Dithering, framebuffer scans, and the DMA staging copy now avoid unnecessary
  PSRAM traffic.

## Adaptive internal ROM arena/cache

The real-device log from the preceding archive reported `arena=0`: requesting
one impossible 256 KiB contiguous block made the entire early reservation fail.
This pass starts Game Boy memory setup before graphics and haptics, then reserves
the largest 16 KiB-aligned block up to 128 KiB while leaving 128 KiB of internal
headroom for later drivers and task stacks.

ROMs that fit run directly from that arena. Larger ROMs remain in PSRAM and use
the same arena as 2 to 8 cached 16 KiB pages. The exact page count is now printed
in every `PERF` line.

## PSRAM bank-cache fallback

Cartridges larger than the arena still use Octal PSRAM at 80 MHz, but the same
arena becomes a guaranteed adaptive internal bank cache.
If the early arena reservation failed, the cache falls back through smaller
internal-heap allocations. Bank 0 remains pinned and the other pages use LRU
replacement.

## Full-speed CPU with half-rate LCD rendering

The previous real-device result was `emu=39 lcd=31 frame=~26000us`. That is not
an FPS counter mismatch: Kirby's CPU was genuinely advancing at only about 65%
speed, which also slowed the timing of its music commands. Meanwhile the Sharp
frontend could consume only about 31 distinct frames per second.

Peanut now skips only its expensive LCD scanline drawing on alternate frames.
CPU, timers, interrupts, joypad and APU register writes still run for every
59.7 Hz Game Boy frame. The visible target remains about 30 FPS, so this does
not lower the real LCD rate seen in the preceding log; it spends the unused
render work on restoring game speed instead. The generic packed renderer also
uses a per-frame dither lookup table and a clipping-free on-screen fast path.

## Scheduling and input safeguards retained from STEP9.3

- BMI270 stays on Core 0, leaving Core 1 to input and Peanut-GB.
- A continuously late emulator yields one real RTOS tick every 64 late frames.
- TCA9555 reads retain the 10 ms timeout and released-sample fail-safe.
- Save writes remain disabled during gameplay and flush on exit or power-off.

## Serial diagnostics

The once-per-second line is now:

```text
PERF emu=... lcd=... frame=...us max=...us ROM=ARENA/INT/PSRAM arena=...
fb=17280 cache=pages:hits/misses miss_us=... audio=buffer/cap under=... over=...
drop=... heap=free/largest i2cerr=...
```

## Acceptance test

Use `1X FAST` for the first performance pass.

1. Startup should report `Reserved adaptive ROM arena` with at least 2 pages.
   A larger ROM should report `ROM=PSRAM`; `cache=4:...` means four pages.
   All games should report `fb=17280` and `LCD render frame skip: ON`.
2. Audio pitch must stay constant. `under=` may expose a genuinely missed
   deadline, but no adaptive stretching should be audible.
3. Target Mario values: `emu=59..61`, `lcd=29..31`.
4. Target Kirby values: `emu=59..61`, `lcd=29..31`, with normal game/music
   tempo. A result near `emu=50` still identifies remaining non-LCD CPU cost.
5. `under=` and `drop=` should stop climbing after startup.
6. No `task_wdt` / `IDLE1` fault and no stuck direction after an I2C error.
7. Mid-tone Game Boy graphics must show the restored ordered-dither texture.

Useful startup lines:

```text
Sharp LS027B7DH01 ready: ... SPI=14000000Hz
Reserved adaptive ROM arena: 65536 bytes (4 cache pages, ...)
PSRAM ROM cache: 4 x 16 KiB (reserved arena)   # page count is device-dependent
LCD render frame skip: ON (manual)
```

## Build and flash

From an ESP-IDF 6.0.2 terminal:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM7 flash monitor
```
