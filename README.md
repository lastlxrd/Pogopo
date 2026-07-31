# STEP7.1 Motion + audio tuning

- Motion Lab numeric values remain raw and immediate.
- Added a visual-only exponential low-pass filter (~170 ms time constant).
- Inverted both visual tilt axes so the ball follows the physical direction of tilt.
- Reduced ball travel, switched to rounded pixel coordinates, and added fixed target rings.
- Reworked the diagonal line into a limited, smooth artificial horizon with a dashed reference.
- Pressing A still zeros the current pose and now also resets the visual filter.
- UI Tick volume: 32 -> 24.
- Error effect volumes: 49/52 -> 40/42.
- Large WAV files are still intentionally rejected above 4 MiB because STEP7 decodes the whole file into PSRAM. Streaming playback is the next storage/audio milestone.
