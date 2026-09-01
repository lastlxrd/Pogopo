# STEP13.3 integration notes

## What this checkpoint merges

- Base: STEP13.2.2 (`main` menu, startup/outro and quick panel).
- Runtime: PogoDate STEP11.6.33 (`playdate-port`).
- The existing `playdate_library` launcher contract is retained. It now maps
  to `PogoDateBrowserApp`, so no STEP13 menu animation code is replaced.
- The Pogopo empty-library page and Game Boy browser remain unchanged.

## Intentional conflict resolutions

- `components/pogopo_app`: keep STEP13.2.2. It owns the animated quick panel,
  power-overlay lifecycle and app quick-action API.
- `components/pogopo_menu` and `components/pogopo_startup`: keep STEP13.2.2.
- `components/pogopo_playdate`: take STEP11.6.33 in full.
- `components/pogopo_audio`: take STEP11.6.33. It is a functional superset
  required by Playdate synth, music and overlapping PCM playback.
- `components/pogopo_graphics`: combine both lines. STEP13 framebuffer loading
  and bitmap scaling stay; STEP11.6.33 native 1x/2x Playdate blits stay.
- `components/pogopo_storage`: take STEP11.6.33 for remount and high-speed
  SDMMC with 20 MHz fallback.
- `main/app_main.cpp`: keep STEP13 lifecycle and register the Playdate browser
  and players alongside the STEP13 apps.

## Recommended Git procedure

This directory is a clean integration snapshot, not a replacement Git history.
On the real repository:

1. Create `integration/main-playdate` from current `main`.
2. Copy this snapshot over that branch and review the resulting diff.
3. Build and test on hardware before merging the integration branch to `main`.
4. Merge `integration/main-playdate` into `main` with a normal merge commit.
5. Retarget or recreate `playdate-port` from the new `main`; continue all
   runtime work there or directly in short feature branches from `main`.

Do not merge the old `playdate-port` into `main` again after this checkpoint;
that would replay obsolete launcher/app-manager changes.

## Hardware smoke test

- Boot animation completes and the STEP13 launcher appears.
- Pogopo, Game Boy, Playdate and Settings tiles animate normally.
- Playdate opens `/playdate`, lists `.pdx` packages and launches one.
- Returning Home stops the game and its audio.
- Power quick panel opens over launcher, Game Boy and Playdate.
- Long Power hold still plays the STEP13 outro forward/reverse correctly.
- SD log reports 40 MHz when the card supports it, otherwise a 20 MHz retry.

The source-only host checks validate the merged C++ interfaces. A complete
firmware build was not run in the preparation environment because ESP-IDF was
not installed there; run `idf.py build` in the project toolchain before merge.
