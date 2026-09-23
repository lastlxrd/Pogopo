#pragma once

#include <cstdint>

namespace pogopo::gfx {

using GlyphColumnFn = uint8_t (*)(char character, int column);

struct Font {
    int glyph_width = 5;
    int glyph_height = 7;
    int spacing = 1;
    GlyphColumnFn column_fn = nullptr;

    uint8_t glyph_column(char character, int column) const {
        if (!column_fn || column < 0 || column >= glyph_width) {
            return 0;
        }
        return column_fn(character, column);
    }
};

const Font& font5x7();

} // namespace pogopo::gfx

