#pragma once

#include <cstdint>

#include "pogopo/gfx/canvas.h"

namespace pogopo::menu {

enum class FontFace : uint8_t {
    Regular14 = 0,
    Italic14,
    Regular22,
    Italic22,
};

class PogoFont {
public:
    static int lineHeight(FontFace face);
    static int textWidth(FontFace face, const char* text);
    static void drawText(gfx::Canvas& canvas, int x, int y, const char* text,
                         FontFace face = FontFace::Regular14,
                         gfx::Color foreground = gfx::BLACK,
                         bool transparent_background = true,
                         gfx::Color background = gfx::WHITE);
};

} // namespace pogopo::menu
