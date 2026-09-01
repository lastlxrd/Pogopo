#include "pogopo/menu/pogo_font.h"

#include <algorithm>

#include "menu_data_generated.h"
#include "pogopo/menu/menu_assets.h"

namespace pogopo::menu {

namespace {

const generated::FontMeta& font_meta(FontFace face) {
    const size_t index = std::min<size_t>(
        static_cast<size_t>(face),
        sizeof(generated::kFonts) / sizeof(generated::kFonts[0]) - 1);
    return generated::kFonts[index];
}

const generated::GlyphMeta* glyph_meta(FontFace face, char character) {
    const uint8_t code = static_cast<uint8_t>(character);
    if (code < generated::kAsciiFirst || code > generated::kAsciiLast) {
        character = '?';
    }
    return &font_meta(face).glyphs[
        static_cast<uint8_t>(character) - generated::kAsciiFirst];
}

} // namespace

int PogoFont::lineHeight(FontFace face) {
    return font_meta(face).line_height;
}

int PogoFont::textWidth(FontFace face, const char* text) {
    if (!text) return 0;
    int line_width = 0;
    int maximum_width = 0;
    while (*text) {
        if (*text == '\n') {
            maximum_width = std::max(maximum_width, line_width);
            line_width = 0;
            ++text;
            continue;
        }
        line_width += glyph_meta(face, *text++)->advance;
    }
    return std::max(maximum_width, line_width);
}

void PogoFont::drawText(gfx::Canvas& canvas, int x, int y, const char* text,
                        FontFace face, gfx::Color foreground,
                        bool transparent_background, gfx::Color background) {
    if (!text || !Assets::valid()) return;
    const uint8_t* base = Assets::base();
    const int origin_x = x;
    while (*text) {
        if (*text == '\n') {
            x = origin_x;
            y += lineHeight(face);
            ++text;
            continue;
        }

        const auto* glyph = glyph_meta(face, *text++);
        if (glyph->offset != 0xFFFFFFFFU && glyph->width > 0) {
            const gfx::Bitmap bitmap = gfx::make_bitmap_1bpp(
                glyph->width, glyph->height, base + glyph->offset,
                gfx::BitOrder::MSB_FIRST);
            canvas.draw_bitmap(x + glyph->x_offset, y, bitmap, foreground,
                               transparent_background, background);
        }
        x += glyph->advance;
    }
}

} // namespace pogopo::menu
