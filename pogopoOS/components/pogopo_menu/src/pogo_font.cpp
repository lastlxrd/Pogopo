#include "pogopo/menu/pogo_font.h"

#include <algorithm>
#include <cstdlib>

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

const generated::GlyphMeta* glyph_meta(FontFace face, uint32_t codepoint) {
    uint8_t code = codepoint <= 0xffU
        ? static_cast<uint8_t>(codepoint) : static_cast<uint8_t>('?');
    if (code < generated::kAsciiFirst || code > generated::kAsciiLast) {
        code = static_cast<uint8_t>('?');
    }
    return &font_meta(face).glyphs[
        code - generated::kAsciiFirst];
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

FontFace PogoFont::closestFace(int target_line_height, bool italic) {
    const FontFace regular_faces[] = {
        FontFace::Regular14, FontFace::Regular22, FontFace::Regular24,
    };
    const FontFace italic_faces[] = {
        FontFace::Italic14, FontFace::Italic22, FontFace::Italic24,
    };
    const FontFace* faces = italic ? italic_faces : regular_faces;
    FontFace best = faces[0];
    int best_distance = std::abs(target_line_height - lineHeight(best));
    for (size_t index = 1; index < 3; ++index) {
        const int distance = std::abs(
            target_line_height - lineHeight(faces[index]));
        if (distance < best_distance) {
            best = faces[index];
            best_distance = distance;
        }
    }
    return best;
}

FontGlyph PogoFont::glyph(FontFace face, uint32_t codepoint) {
    const auto* meta = glyph_meta(face, codepoint);
    const uint8_t* base = Assets::base();
    if (!meta || !base) return {};
    FontGlyph result{};
    result.width = meta->width;
    result.height = meta->height;
    result.stride = static_cast<uint8_t>((meta->width + 7U) / 8U);
    result.x_offset = meta->x_offset;
    result.advance = meta->advance;
    if (meta->offset != 0xFFFFFFFFU && meta->width > 0U) {
        result.bitmap = base + meta->offset;
    }
    return result;
}

void PogoFont::drawText(gfx::Canvas& canvas, int x, int y, const char* text,
                        FontFace face, gfx::Color foreground,
                        bool transparent_background, gfx::Color background) {
    if (!text || !Assets::valid()) return;
    const int origin_x = x;
    while (*text) {
        if (*text == '\n') {
            x = origin_x;
            y += lineHeight(face);
            ++text;
            continue;
        }

        const FontGlyph glyph = PogoFont::glyph(
            face, static_cast<uint8_t>(*text++));
        if (glyph.bitmap && glyph.width > 0) {
            const gfx::Bitmap bitmap = gfx::make_bitmap_1bpp(
                glyph.width, glyph.height, glyph.bitmap,
                gfx::BitOrder::MSB_FIRST);
            canvas.draw_bitmap(x + glyph.x_offset, y, bitmap, foreground,
                               transparent_background, background);
        }
        x += glyph.advance;
    }
}

} // namespace pogopo::menu
