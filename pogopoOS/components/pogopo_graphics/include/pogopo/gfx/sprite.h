#pragma once

#include "pogopo/gfx/bitmap.h"

namespace pogopo::gfx {

struct Sprite {
    Bitmap bitmap{};
    int x = 0;
    int y = 0;
    Color foreground = BLACK;
    Color background = WHITE;
    bool transparent_background = true;
    bool visible = true;

    Rect bounds() const { return {x, y, bitmap.width, bitmap.height}; }
};

} // namespace pogopo::gfx

