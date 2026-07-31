# pogopoOS2.0 — Graphics Step 3

This build keeps the working Sharp LS027B7DH01 transport from the previous commit and adds a structured graphics layer.

## Public API

```cpp
#include "pogopo/gfx/gfx.h"

pogopo::Graphics gfx;
gfx.begin(config);
gfx.clear();
gfx.drawText(10, 10, "hello");
gfx.drawSprite(sprite);
gfx.present();
```

Everything also exists in the explicit `pogopo::gfx` namespace.

## Component layout

```text
components/pogopo_graphics/
├── include/pogopo/gfx/
│   ├── types.h
│   ├── bitmap.h
│   ├── sprite.h
│   ├── font.h
│   ├── sharp_display.h
│   ├── canvas.h
│   ├── graphics.h
│   └── gfx.h
└── src/
    ├── drivers/sharp_display.cpp
    ├── canvas/canvas.cpp
    ├── font/font5x7.cpp
    └── core/graphics.cpp
```

## Included in this commit

- `pogopo::gfx` namespace and friendly top-level aliases
- `Graphics` facade (`gfx.drawLine`, `gfx.drawText`, `gfx.present`)
- `Canvas` drawing layer
- `Rect` clipping
- 1-bit `Bitmap` with MSB/LSB bit order
- `Sprite`
- standalone `Font` abstraction and built-in 5x7 font
- Sharp driver, PSRAM framebuffer, dirty rows and VCOM task preserved
- test screen with a sprite deliberately crossing the clipping bounds
