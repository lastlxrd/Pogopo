# STEP11.6.5 hardware check

Build this directory from a clean ESP-IDF build tree and confirm both serial
identifiers before judging the result:

```text
pogopoOS2.0 STEP11.6.5 POGODATE GRIDVIEW IMU
PogoDate API STEP11.6.5: bundled Gridview + Playdate-axis BMI270
```

## Maze

1. Open Maze and choose **LEVELS**. Move through several rows and sections.
   The list must scroll with section headers and must not report
   `setSectionHeaderHeight` or another Gridview error.
2. Start a level on a stationary, level console. If the game offers its own
   calibration flow, calibrate in that position.
3. Tilt the right edge down: the marble must accelerate right. Tilt the left
   edge down: it must accelerate left.
4. Tilt the lower edge down: the marble must accelerate down. Tilt the upper
   edge down: it must accelerate up.
5. Return to level and reverse direction repeatedly. There should be no long
   delayed motion; only a short sensor-noise filter is active.

## Duel Of Shadows

1. Press A or physical START on the title screen.
2. In the tutorial room, A dashes and B attacks; holding B charges the spin.
3. Face right and use repeated A dashes toward the right-side portal. This is
   the game's intended room transition; ordinary running at the edge is not.
4. Confirm the boss room appears and the player, boss, effects, music and
   attacks remain visible and responsive.

If a check fails, attach the first complete Lua error plus one PERF line and,
for Maze motion, state which physical edge was tilted down. That separates an
API failure from a board-axis or old-calibration issue. STEP11.6.5 PERF lines
include the mapped `acc=x,y,z` sample for this purpose.
