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
| separate `.pdz` module | Supported | `import` can open root or nested module archives; `playdate.file.load()`/`run()` return or execute standalone PDZ chunks such as Pulp `data.pdz`. |
| `.pdi` image | Supported | Bitmap, transparent-mask and trimmed-cell data are decoded from the package. |
| `.pdt` image table | Supported | Frames are decoded on demand and retained by the Lua table cache. |
| `.pft` font | Supported | Glyph pages, masks, UTF-8 lookup, tracking and kerning are used. |
| `.pda` PCM/IMA ADPCM | Supported subset | 8/16-bit mono/stereo PCM plus mono/stereo IMA are decoded; stereo is downmixed and long tracks use the music path. |
| `pdex.bin` native game | Not supported | It contains ARM Cortex-M7 machine code, not ESP32-S3 Xtensa code. |
| ZIP download wrapper | Extract first | Put the resulting `.pdx` directory under `/playdate/` on the SD card. |

STEP11.6.19 adds SDK-style `graphics.font.newFamily()` tables with normal,
bold and italic PFT faces, family selection and measurement, per-font
tracking/leading, global tracking, font-object drawing/alignment, glyph image
extraction and bounded text sizing. These are generic runtime APIs rather than
Hillslide-specific substitutions.

STEP11.6.20 aligns PogoDate's Lua class system with `CoreLibs/object.lua`:
property-backed and namespaced classes, class metadata, parent allocation,
concrete class links on instances, `isa`, inherited metamethods and callable
scene reconstruction are supported. This fixes class-driven scene managers
without making arbitrary Lua tables callable.

STEP11.6.21 adds the SDK's `playdate.timer.keyRepeatTimer()` and
`keyRepeatTimerWithDelay()` timing/callback behavior. It also follows
Playdate's system-font fallback when a custom PFT lacks an ASCII decoration or
common directional symbol, preventing menu arrows and brackets from becoming
question marks.

STEP11.6.22 completes every API symbol statically referenced by Hillslide's 35
Lua modules. Sample/file players expose playback offset and volume, one-shot
players finish according to PDA duration, stencil clearing accepts the SDK's
`clearStencilImage()` name, scoreboards preserve asynchronous callbacks with
offline results, and compiled image resources recover filename-only case
differences such as `avalanche` versus `Avalanche.pdt`.

STEP11.6.23 fixes standard timer callback varargs and the real two-dimensional
shape of compiled PDT image tables. SD PDT data stays packed until a frame is
requested; frames remain trimmed 1-bit images behind a bounded cache and are
expanded to mutable pixels only on demand. This prevents animation-heavy games
from retaining every inflated table and full transparent frame canvas.

STEP11.6.24 removes repeated PDT inflation and full-GC fallback from Hillslide.
Expanded PDT streams now live in a 1.4 MiB PSRAM LRU instead of being inflated
again for every new frame, while returned images remain compact independent
copies. The compositor parses each compiled 1-bit frame once per draw instead
of once per pixel, and large render-target overflow no longer forces a
stop-the-world Lua collection. This step also adds keyboard lifecycle,
mic/headset capability detection, display mosaic state, display flipping and
direct display-image loading compatibility. Hardware logs later proved that
PDT decode was only 6–67 ms and that the remaining 1.18–1.32 second stalls were
inside terrain geometry generation.

STEP11.6.25 moves `drawPolygon`, `fillPolygon`, `drawTriangle` and
`fillTriangle` from the Lua fallback into the native runtime. Polygon scan
conversion is clipped before rasterization and patterned Copy-mode spans write
directly into render targets. This targets Hillslide's seven-pass dithered hill
generator without package-specific patches while preserving the SDK numeric
and `geometry.polygon` overloads.

STEP11.6.26 corrects `graphics.setDrawOffset()` semantics. The offset is now
applied to images, scaled/rotated images, rectangles, lines, pixels, circles,
polygons, triangles and font glyphs before clipping. It is no longer added to
the already clipped full-screen framebuffer during LCD flush. World-space
objects beyond x=400 therefore enter the camera viewport correctly, while
`image:drawIgnoringOffset()` and `sprite:setIgnoresDrawOffset()` remain fixed
to screen coordinates.

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
image scaling and draw modes, image masks, vector geometry, sprite collision
metadata and accelerometer input. One scripted run opens LEVELS and scrolls
its multi-section grid for 540 frames; a second drives a live level for 900
frames; a third drops the ball into a hole and runs through `LevelComplete`
for 780 frames. All complete with zero Lua errors. Hardware maps the
Pogopo-mounted BMI270 to Playdate's +X-right/+Y-down/+Z-through-display
coordinates with a low-latency IIR filter and held-last-good-sample behavior.
Physical left/right buttons use the common Pogopo mapping. Maze 1.1.0 has a
bundle-scoped horizontal-menu quirk and a bundle-scoped accelerometer Y
correction; neither changes any other PDX. Maze's saved neutral calibration
must be recreated after flashing STEP11.6.10. Its 324x137 completion render
target is pinned and reused, while decoded sparse image-table frames retain
their real content bounds so transparent canvas pixels are not scanned on every
draw. The exact 600-frame animation stays pixel-identical to STEP11.6.9 and is
about 8.1x faster in the host regression. Maze also opts out of Pogopo's
START-to-A convenience alias, preventing an extra START press from confirming a
result-screen choice.

### Duel Of Shadows

Validated against the attached free Lua PDX without modifying its package files
or assets. It exercises JSON and LDtk loading, source-name to compiled-resource
mapping, a real PFT font, native clipped tilemap sprites, timer overloads and callbacks,
easing, overlap queries, subclass sprite defaults, draw-offset screen shake,
large sample handling and stereo IMA ADPCM music. A scripted run leaves the
title, uses the A-button dash to cross the tutorial portal, renders and plays
the boss fight, then survives the player-death save/return path. Sprite area
queries detect added sprites geometrically even while a game has disabled
their physical collision response, which is how the dash portal senses the
player. Scaled sprites preserve horizontal image flips, automatically follow
new animation-frame dimensions unless the game explicitly called `setSize()`,
and use a source-driven nearest-neighbor scaler. Swept rectangle contacts avoid
the previous far-edge ejection when actors start a frame overlapping. The long
run produces 1,800 logical frames and 56 audio starts with zero Lua errors; a
second direct boss-room regression produces 1,800 frames and 71 audio starts.
A separate synthetic package keeps only
`main` in `main.pdz` and resolves the remaining modules from separate nested
`.pdz` archives, verifying that path independently.

### Existing regressions

PDSnake and both the embedded-source and SD-PDX versions of Celeste Classic
remain in the regression set. This matters because their tilemap, collision,
Pico-8 font, persistent image layers and timer usage differ from the two games
above.

### Hillslide

Validated against the supplied 35-module Lua PDX without modifying its package
or redistributing it. The host suite opens all nine scenes and exercises menu
SFX reuse, three PFT fonts, gridview, QR CoreLibs, scene classes, Training,
main gameplay, avalanche resources, Recap and score submission. A dedicated
probe validates playback offsets, volume, one-shot completion, looping music,
stencil clearing and asynchronous scoreboard callbacks. Training runs 500
frames and the main game 1,000 frames with zero Lua errors.

### Synth API probe

A one-module Lua `main.pdz` probe covers the startup path that previously failed
because `playdate.sound.synth` was nil. It creates and copies synths, validates
the eight waveform constants, plays Hertz, MIDI and named notes, changes
waveforms and transpose, applies ADSR/volume/parameters, then exercises
`noteOff()` and `stop()`. The 300-frame run produces four managed tones with
zero Lua errors. The user's separate game is not redistributed with the test.

### Crank API probe

A one-module Lua probe covers every documented crank query, sound-setting,
callback, input-handler and standard indicator entry point. A host-only input
source advances the absolute angle through three 30-degree steps, verifies
positive ticks and both accelerated/non-accelerated change values, routes one
movement through the global callback and one through an exclusive input
handler, then checks dock and undock callbacks. The 300-frame run completes
with zero Lua errors. Pogopo hardware without a crank module stays at the
neutral extended/0-degree/no-movement state.

### External PDZ load probe

A two-file synthetic Pulp-style package keeps its runtime in `main.pdz` and a
returned data table in `data.pdz`. The 200-frame regression validates
`playdate.file.load("data")`, the explicit `.pdz` suffix, a caller-supplied
`_ENV`, `playdate.file.run()`, a missing-file `(nil, error)` result and the
package's authored 20 FPS rate. The supplied `WOOD` hardware log reaches this
same deferred-load boundary after its one Lua module starts successfully.

## Currently implemented API areas

- display size, scale, offset, inversion and per-package refresh rate, with
  state getters backed by the native renderer;
- buttons, just-pressed/released state and input-handler stack, using the
  same physical left/right mapping as PogopoOS plus a bundle-scoped Maze
  menu-direction quirk;
- all documented crank queries and callbacks, crank input-handler routing,
  sound-disable state and the shared UI crank indicator; hardware without a
  crank module reports an extended crank at 0 degrees with zero movement;
- images with fractional drawing, sparse content bounds and generated scaling,
  image tables with
  `table[index]`/`drawImage()` access, image masks, PFT fonts, all eight image
  draw modes, bitmap and ordered-dither patterns, clipping, contexts, focus
  locking, rectangles, circles, ellipses/arcs, triangles, polygons, text,
  system/current-font lookup, missing-PFT system glyph fallback, circled A/B
  button symbols and common directional-symbol fallback, rotated/faded
  drawing, anchored/cropped/masked drawing, generated rotated
  and affine-transformed images, and reusable text images;
- Playdate's public `kColorBlack=0`, `kColorWhite=1`, `kColorClear=2` and
  `kColorXOR=3` values, translated to PogoDate's private pixel representation;
- sprite ordering, subclass defaults, visibility, image/center/scale/rotation/
  flip/clip state, automatic animation-frame sizing, object `isa()`, geometric
  area/overlap/point/line queries, ordered line-hit metadata, swept collision
  contacts and normals, group and collides-with bitmasks, tilemap-backed
  sprites, mutable tilemaps, merged tile collision rects and correctly-sized
  tilemap walls;
- display offset and graphics draw offset, including draw-offset screen shake;
- callback/value millisecond timers, including SDK key-repeat timers;
  explicit callback varargs are preserved, while callbacks without arguments
  receive their timer object;
  callback/value frame timers with easing, repeats and reverses; bundled
  easing, animation and animator CoreLibs plus elapsed-time helpers;
- Playdate date/time conversion forms and cycle-safe table copy helpers;
- geometry rect, point, size, vector, line-segment, polygon, arc and affine
  transform types, including intersections, bounds, containment, distances,
  translation and transformed AABBs;
- weighted A* pathfinding graphs and nodes, including connection mutation,
  ID/coordinate lookup, 2D-grid creation and node or ID path results;
- accelerometer start/stop/state/read APIs, fed by Pogopo's BMI270 on hardware;
- Pogopo START-to-Playdate-A translation while a PDX owns input, except for the
  Maze bundle where START must remain independent of result-screen A actions;
- sandboxed files and datastore under each package bundle ID;
- deferred standalone PDZ loading/running through `playdate.file.load()` and
  `playdate.file.run()`, including optional custom `_ENV` tables;
- the standard Pulp runtime's class-style image draw/clear/tiled calls,
  pixel drawing and resettable audio clock;
- Playdate's button first-responder cascade, including pushed input handlers,
  masking handlers and the global `playdate` callback table used by Pulp;
- JSON string/file decoding plus compact, pretty and file encoding;
- short sample effects, stereo/mono PCM and IMA decoding, basic playback rate,
  duration metadata, playback offset/volume queries, one-shot completion,
  pause/resume position and a separate music player;
- packed, correctly shaped PDT tables with a bounded expanded-table PSRAM LRU,
  compact trimmed-frame caching and on-demand mutable materialization;
- stack-safe iterative PDX sound discovery with its bounded directory queue and
  path scratch storage in PSRAM; startup stack high-water diagnostics are
  emitted for new package testing;
- managed Lua synth voices with sine, square, triangle, noise and sawtooth
  oscillators; Hertz/MIDI/named-note input; independent note release/stop;
  volume, transpose and ADSR shaping;
- package metadata and mutable system-menu items with values and callbacks;
- asynchronous offline scoreboard callback compatibility for score submission
  and empty score queries.

## Coverage snapshot

The Playdate SDK 3.0.5 Lua reference contains 267 documented function entries.
After excluding the five crank queries requested outside this target, PogoDate
has usable implementations or compatibility behaviour for roughly 226–236 of
262 entries. About 26–36 callable entries (10–14%) therefore remain, mostly in
networking, keyboard UI, microphone/video, simulator/debug services and the
advanced sound graph. This is an endpoint estimate rather than a promise of
bit-exact semantics: collision, audio mixing and rendering edge cases can still
need fidelity work even when their functions exist. Hillslide itself is at
100% of statically referenced API symbols after STEP11.6.22.

## Known limitations

- API coverage is intentionally incomplete. A new Lua game can still stop on
  the first unimplemented Playdate function or on CoreLib behavior beyond this
  compatibility layer.
- Pulp's runtime can load its separate `data.pdz`, cache its image tables and
  enter gameplay. PulpScript game logic is covered by the same runtime rather
  than by package-name workarounds, but less common SDK calls can still expose
  another unsupported API in a future package.
- Sprite collision response covers overlap, slide, freeze and basic bounce,
  accepts callback and direct-string response forms, and returns swept
  normal/move/touch metadata. It is not yet a byte-for-byte replacement for
  every edge case in Playdate's native solver.
- Networking and SDK extensions are not emulated by this step. Microphone and
  headset capability calls exist and correctly report absent hardware, but
  Pogopo cannot record audio without a microphone source.
  The Lua crank API is implemented, but Pogopo has no built-in physical crank;
  crank-driven gameplay therefore remains stationary until an expansion
  module supplies angle/dock state. Accelerometer axes and filtering are implemented for Pogopo's
  BMI270 orientation, but Maze's saved neutral point must be recalibrated on
  the device after flashing STEP11.6.25.
- Oscillator synths are implemented, but Playdate's PO waveforms are currently
  approximated. Sample/wavetable synthesis, signal/LFO modulation, exact
  scheduled `when` events, finish callbacks, instruments and sequences are not
  complete. File-player position is tracked, but true streamed decoding and
  exact loop subranges are not implemented; long PDA music is decoded into
  PSRAM when started.
- Scoreboard callbacks are API-compatible offline placeholders. They do not
  contact Panic's servers or provide global rankings.
- Affine-transformed image generation uses a correct inverse-mapped software
  path, but large images or per-frame transforms can be expensive on ESP32-S3.
  Video, QR generation and several specialized bitmap filters are incomplete.
- The Pogopo shell does not yet draw Playdate's system-menu overlay. Menu items
  and callbacks are functional for game logic, but their presentation remains
  owned by the future shell integration.
- The ESP32-S3 and Playdate have different CPU, memory and peripheral budgets.
  Passing the host regression is necessary, but final frame time, audio and SD
  reliability must still be checked on Pogopo hardware.
- The PogoDate loader runs inside the 12 KiB `pogopo_os` task. Recursive asset
  scanning has been removed, but unusually deep Lua/C callback recursion can
  still consume that finite native stack; startup logs expose its minimum free
  value for hardware reports.

## Testing another free game

1. Confirm the download is free to obtain and read its redistribution/source
   license separately. Do not add paid game files to the repository.
2. Extract the download so `/playdate/<Game>.pdx/pdxinfo` and `main.pdz` exist.
3. Launch it from `Playdate games (SD)` and keep the first complete Lua error.
4. Add an API only when its real semantics are understood; avoid per-game
   screen-coordinate or asset substitutions.
5. Re-run Onebit Frogger, Godspeed, PDSnake and Celeste regressions after every
   compatibility change.
