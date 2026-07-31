# pogopoOS2.0 STEP4 — Input + Haptics

This build keeps the working `pogopo::gfx` previous commit engine and adds two native
ESP-IDF components:

- `pogopo_input`: persistent TCA9555 device, active-low buttons, 4 ms polling,
  GPIO21 interrupt wake-up, per-button debounce, held/pressed/released,
  repeat, long-press and a FreeRTOS event queue.
- `pogopo_haptics`: asynchronous GPIO3 vibration motor task with queued effects.

## Test controls

- D-pad: move the sprite.
- A: click vibration.
- B: double-click vibration.
- Start: center the sprite + confirm pattern.
- Hold Menu for 700 ms: alert pattern.
- A+B together: heavy pattern.

The screen shows held/raw masks, the last event, input queue/error counters,
vibration state, FPS and Sharp dirty-row transfer statistics.

## Button mapping

- P8 Top
- P9 Down
- P10 Left
- P11 Right
- P12 B
- P13 A
- P14 Menu
- P15 Start

All buttons are active LOW. The vibration motor is active HIGH on GPIO3.
