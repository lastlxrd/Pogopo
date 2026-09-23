#pragma once

#include <cstdint>

#include "pogopo/gfx/canvas.h"

namespace pogopo::menu {

enum class FontFace : uint8_t {
    Regular14 = 0,
    Italic14,
    Regular22,
    Italic22,
    Regular24,
    Italic24,
};

struct FontGlyph {
    const uint8_t* bitmap = nullptr;
    uint8_t width = 0;
    uint8_t height = 0;
    uint8_t stride = 0;
    int8_t x_offset = 0;
    uint8_t advance = 0;
};

class PogoFont {
public:
    static int lineHeight(FontFace face);
    static int textWidth(FontFace face, const char* text);
    static FontFace closestFace(int target_line_height, bool italic = false);
    static FontGlyph glyph(FontFace face, uint32_t codepoint);
    static void drawText(gfx::Canvas& canvas, int x, int y, const char* text,
                         FontFace face = FontFace::Regular14,
                         gfx::Color foreground = gfx::BLACK,
                         bool transparent_background = true,
                         gfx::Color background = gfx::WHITE);
};

} // namespace pogopo::menu
