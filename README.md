# pogopoOS2.0 — STEP9 Game Boy

This step integrates a native Game Boy application into the existing Pogopo app framework.
It is not the old Arduino monolith: emulation, display, input, audio, storage, saves and UI
remain separate components.

## What is included

- `pogopo_gameboy` component based on Peanut-GB and MiniGB APU.
- `/gameboy` ROM browser on the SD card.
- `.gb` DMG ROM loading from SD into internal RAM or PSRAM.
- 8 x 16 KiB internal-ROM cache when a ROM lives in PSRAM.
- Emulator task on Core 1 at approximately 59.7 emulated frames per second.
- Sharp LCD output at about 30 submitted frames per second through Peanut frame skip.
- Two display modes:
  - `FIT 240`: 160x144 -> 267x240, centred.
  - `1X FAST`: native 160x144, centred; useful when maximum display speed matters.
- Four Game Boy shades converted to the monochrome Sharp display with ordered dithering.
- Realtime stereo audio ring mixed by the existing Core 0 I2S audio task.
- A small realtime resampler fixes the slight MiniGB-per-frame sample-rate mismatch.
- Battery-backed RAM saves beside the ROM as `.sav`.
- Periodic save flush and flush again when leaving a game or powering off.
- 240 MHz CPU configuration.
- Requested haptics tuning: `Tick = 55 ms`, `Click = 64 ms`.

## SD card layout

Put uncompressed original Game Boy ROMs here:

```text
/gameboy/
    Tetris.gb
    Super Mario Land.gb
    Zelda.gb
```

No ROM files are included in this archive.

Supported in this first port:

- `.gb` files
- classic monochrome Game Boy / DMG software
- ROM sizes up to 8 MiB
- supported Peanut-GB cartridge controllers

Not supported yet:

- `.gbc`
- `.zip`
- link cable
- save states
- RTC persistence for clock-based cartridges

## Controls in ROM browser

```text
TOP / DOWN     select ROM
A              launch ROM
LEFT / RIGHT   switch FIT 240 / 1X FAST
START          rescan /gameboy
B              return to Launcher
```

## Controls in a game

```text
D-pad          Game Boy D-pad
A / B          Game Boy A / B
START          Game Boy Start
MENU           Game Boy Select
B + START      hold about 0.65 s to save and exit
```

`MENU` is reserved as Select while the emulator is running, so the normal Pogopo system
overlay is intentionally disabled inside a Game Boy game.

## First test order

1. Start with a small, known-good `.gb` ROM such as a simple puzzle/platform title.
2. Test `1X FAST` first if `FIT 240` feels visually slower.
3. Check that audio remains smooth while moving through a busy scene.
4. Make an in-game save, exit with `B + START`, then launch again and verify the save.
5. Watch the serial log for ROM location, cache page count, save size and errors.

## Architecture

```text
Core 0:
    pogopo_audio -> I2S DMA -> MAX98357A
    UI sounds + Game Boy stereo mixer

Core 1:
    Peanut-GB CPU/LCD frame loop
    MiniGB APU frame generation

OS task:
    input mapping
    app lifecycle
    latest-frame copy
    Sharp framebuffer + dirty-row present
```

The display driver remains at the known-good 2 MHz Sharp configuration. `FIT 240` can touch
all 240 panel rows during a busy frame, so the emulator continues near 60 Hz independently
while the display presents the newest available frame. This prevents video traffic from
slowing the emulated CPU or starving audio.

## Third-party code

See `components/pogopo_gameboy/THIRD_PARTY.md`. Peanut-GB and MiniGB APU source notices are
preserved. The Pogopo frontend, FreeRTOS task split, PSRAM/cache strategy, Sharp renderer,
input mapping, save handling and realtime audio bridge are project code.
