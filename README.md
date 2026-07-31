# STEP9.1 Game Boy performance / compatibility patch

Changes:
- Core 1 is dedicated to Peanut-GB; OS/render/IMU/power run on Core 0.
- Peanut-GB frame skip disabled for compatibility (notably games that stalled visually).
- Frontend publishes every second LCD frame, keeping a 30 FPS display target.
- Game Boy frame buffers prefer internal RAM instead of PSRAM.
- New row-packed Sharp renderer avoids per-pixel drawing calls.
- 1X FAST is now the default because the 2 MHz Sharp link can sustain it near 30 FPS.
- FIT 240 remains available but is physically limited to roughly 20 FPS by full-frame SPI traffic.
- Serial monitor prints separate EMU FPS and LCD FPS once per second.

Expected serial line:
`PERF emu=59 fps lcd=29 fps frame=... cache=... audioDrop=0`
