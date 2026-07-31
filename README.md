# pogopoOS2.0 STEP5 — GUI + Application Framework

STEP5 keeps the proven previous commit graphics/input/haptics stack and adds two native
ESP-IDF components:

- `pogopo_gui`: Theme, Widget, Label, Panel, ProgressBar, List, Dialog, header,
  footer and wrapped-text helpers.
- `pogopo_app`: Application lifecycle, AppContext, AppManager, dirty-region
  invalidation, launcher switching and a modal system menu.

## Demo applications

- **Launcher** — move with Top/Down, open with A.
- **Graphics demo** — animated sprite and progress bar; A pauses, B returns.
- **Input monitor** — live button state, raw TCA9555 value and counters.
- **Haptics lab** — select and play every vibration pattern.
- **About** — framework information.

Press **Menu** in any application to open the system overlay. Use Top/Down and A,
or close it with B/Menu.

## Components

```text
components/
  pogopo_graphics/
  pogopo_input/
  pogopo_haptics/
  pogopo_gui/
  pogopo_app/
```

The main task runs on core 1. Input, VCOM and haptics stay asynchronous on core 0.
The GUI only refreshes when an application invalidates a region, so idle screens
produce no unnecessary display traffic.
