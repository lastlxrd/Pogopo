# PogoDate compatibility status

PogoDate is a focused Lua Playdate compatibility runtime for Pogopo's
ESP32-S3. It is not Playdate OS and it does not emulate the ARM CPU. The goal
is to run legally obtained Lua `main.pdz` packages directly from an extracted
`.pdx` directory while mapping the APIs they use onto Pogopo's 400 x 240 1-bit
display, controls, audio and SD storage.

## Package boundary

| Package content | Status | Notes |
| --- | --- | --- |
| `main.pdz` Lua game | Supported subset | Playdate's Lua opcode order is normalized before stock Lua 5.4 executes it. |
| separate `.pdz` module | Supported | `import` can open root or nested module archives and satisfy their internal imports. |
| `.pdi` image | Supported | Bitmap, transparent-mask and trimmed-cell data are decoded from the package. |
| `.pdt` image table | Supported | Frames are decoded on demand and retained by the Lua table cache. |
| `.pft` font | Supported | Glyph pages, masks, UTF-8 lookup, tracking and kerning are used. |
| `.pda` PCM/IMA ADPCM | Supported subset | 8/16-bit mono/stereo PCM plus mono/stereo IMA are decoded; stereo is downmixed and long tracks use the music path. |
| `pdex.bin` native game | Not supported | It contains ARM Cortex-M7 machine code, not ESP32-S3 Xtensa code. |
| ZIP download wrapper | Extract first | Put the resulting `.pdx` directory under `/playdate/` on the SD card. |

## Validated games

### Onebit Frogger

Validated against `onebitfrogger_20220716.pdx`. The title animation, gameplay
assets, native PFT text, input, save file and PDA effects run with the game's
requested 20 FPS update rate. The host regression runs 240 logical frames with
zero Lua errors and checks captured 400 x 240 title/gameplay frames.

### Godspeed

Validated as the second external package because the game is free and its
repository explicitly licenses the code under MIT and assets under CC BY 4.0.
It exercises 43 compiled Lua modules, a custom PFT font, an 800 x 240 scrolling
background, frame timers, sprite collisions and rotation, datastore, text
alignment and multiple sounds. The host regression runs 300 logical frames at
30 FPS with zero Lua errors; a longer visual run reaches active gameplay,
collisions, difficulty changes and game over.

### Maze

Validated against the supplied 53-module Lua PDX without changing its package
files. Maze exercises Noble Engine settings and scenes, system and compiled
fonts, the package's exact bundled `CoreLibs/ui/gridview`, bundled
easing/animation CoreLibs, image-table indexing,
image masks, vector geometry, sprite collision metadata and accelerometer
input. One scripted run opens LEVELS, draws and scrolls its multi-section grid
for 540 frames; a second enters a live level, injects changing accelerometer
data for 3,000 host ticks and produces 900 logical frames plus 11 audio starts.
Both complete with zero Lua errors. Hardware maps the Pogopo-mounted BMI270 to
Playdate's +X-right/+Y-down/+Z-through-display coordinates with a low-latency
IIR filter and held-last-good-sample behavior. Directional feel and the game's
own neutral calibration still require a play test after flashing.

### Duel Of Shadows

Validated against the attached free Lua PDX without modifying its package files
or assets. It exercises JSON and LDtk loading, source-name to compiled-resource
mapping, a real PFT font, tilemap sprites, timer overloads and callbacks,
easing, overlap queries, subclass sprite defaults, draw-offset screen shake,
large sample handling and stereo IMA ADPCM music. A scripted run leaves the
title, uses the A-button dash to cross the tutorial portal, renders and plays the boss fight, then
survives the player-death save/return path. It produces 1,800 logical frames
and 50 audio starts with zero Lua errors. A second synthetic package keeps only
`main` in `main.pdz` and resolves the remaining modules from separate nested
`.pdz` archives, verifying that path independently.

### Existing regressions

PDSnake and both the embedded-source and SD-PDX versions of Celeste Classic
remain in the regression set. This matters because their tilemap, collision,
Pico-8 font, persistent image layers and timer usage differ from the two games
above.

## Currently implemented API areas

- display size, scale, offset, inversion and per-package refresh rate;
- buttons, just-pressed/released state and input-handler stack;
- images, image tables with `table[index]` access, image masks, PFT fonts, draw
  modes, bitmap and ordered-dither patterns, clipping, contexts, focus
  locking, primitives, text,
  system/current-font lookup and rotated/faded drawing;
- Playdate's public `kColorBlack=0`, `kColorWhite=1`, `kColorClear=2` and
  `kColorXOR=3` values, translated to PogoDate's private pixel representation;
- sprite ordering, subclass defaults, visibility, image/center/scale/rotation/
  clip state, object `isa()`, overlap queries, collision normals, group
  filters, tilemap-backed sprites and tilemap walls;
- display offset and graphics draw offset, including draw-offset screen shake;
- callback/value millisecond timers; callback/value frame timers with easing,
  repeats and reverses; bundled easing, animation and animator CoreLibs plus
  elapsed-time helpers;
- Playdate date/time conversion forms and cycle-safe table copy helpers;
- accelerometer start/stop/state/read APIs, fed by Pogopo's BMI270 on hardware;
- Pogopo START-to-Playdate-A translation while a PDX owns input;
- sandboxed files and datastore under each package bundle ID;
- JSON string/file decoding into Lua tables;
- short sample effects, stereo/mono PCM and IMA decoding, basic playback rate,
  duration metadata and a separate music player;
- package metadata and minimal system-menu hooks.

## Known limitations

- API coverage is intentionally incomplete. A new Lua game can still stop on
  the first unimplemented Playdate function or on CoreLib behavior beyond this
  compatibility layer.
- Sprite collision response now covers the overlap, slide and freeze behavior
  used by the validated games and returns normal/move/touch metadata. It is not
  yet a complete swept Playdate solver, and bounce fidelity remains limited.
- Crank input, networking, microphone and SDK extensions are not emulated by
  this step. Accelerometer axes and filtering are implemented for the BMI270
  orientation used by Pogopo, but still require on-device direction and
  calibration validation.
- Audio playback supports the package formats and basic controls used by the
  test set, not every Playdate synth/effect/sequence API. File-player seeking,
  true streamed decoding and loop subranges are not implemented yet; long PDA
  music is decoded into PSRAM when started.
- JSON decode/decodeFile are implemented; JSON encoding is not yet exposed.
- The ESP32-S3 and Playdate have different CPU, memory and peripheral budgets.
  Passing the host regression is necessary, but final frame time, audio and SD
  reliability must still be checked on Pogopo hardware.

## Testing another free game

1. Confirm the download is free to obtain and read its redistribution/source
   license separately. Do not add paid game files to the repository.
2. Extract the download so `/playdate/<Game>.pdx/pdxinfo` and `main.pdz` exist.
3. Launch it from `Playdate games (SD)` and keep the first complete Lua error.
4. Add an API only when its real semantics are understood; avoid per-game
   screen-coordinate or asset substitutions.
5. Re-run Onebit Frogger, Godspeed, PDSnake and Celeste regressions after every
   compatibility change.
