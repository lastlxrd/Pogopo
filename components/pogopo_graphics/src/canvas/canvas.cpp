#include "pogopo/gfx/canvas.h"

#include <algorithm>
#include <cstdlib>

namespace pogopo::gfx {

Canvas::Canvas(SharpDisplay& display) : display_(display) {
    reset_clip();
}

void Canvas::set_clip(const Rect& rect) {
    clip_ = Rect::intersect(rect, {0, 0, width(), height()});
}

void Canvas::reset_clip() {
    clip_ = {0, 0, width(), height()};
}

Rect Canvas::clipped_rect(int x, int y, int w, int h) const {
    return Rect::intersect({x, y, w, h}, clip_);
}

void Canvas::set_pixel_unlocked(int x, int y, Color color) {
    if (inside_clip(x, y)) {
        display_.set_pixel_unlocked(x, y, color);
    }
}

void Canvas::clear(Color color) {
    display_.clear(color);
}

void Canvas::clear_clip(Color color) {
    const Rect r = clip_;
    if (r.empty()) return;
    display_.lock();
    display_.fill_rect_unlocked(r.x, r.y, r.w, r.h, color);
    display_.unlock();
}

void Canvas::draw_pixel(int x, int y, Color color) {
    if (!inside_clip(x, y)) return;
    display_.draw_pixel(x, y, color);
}

void Canvas::draw_hline(int x, int y, int w, Color color) {
    if (w <= 0 || y < clip_.y || y >= clip_.bottom()) return;
    const Rect r = clipped_rect(x, y, w, 1);
    if (r.empty()) return;
    display_.lock();
    display_.draw_hline_unlocked(r.x, r.y, r.w, color);
    display_.unlock();
}

void Canvas::draw_vline(int x, int y, int h, Color color) {
    if (h <= 0 || x < clip_.x || x >= clip_.right()) return;
    const Rect r = clipped_rect(x, y, 1, h);
    if (r.empty()) return;
    display_.lock();
    for (int row = r.y; row < r.bottom(); ++row) {
        display_.set_pixel_unlocked(x, row, color);
    }
    display_.unlock();
}

void Canvas::draw_line(int x0, int y0, int x1, int y1, Color color) {
    display_.lock();
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        set_pixel_unlocked(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = error * 2;
        if (e2 >= dy) { error += dy; x0 += sx; }
        if (e2 <= dx) { error += dx; y0 += sy; }
    }
    display_.unlock();
}

void Canvas::draw_rect(int x, int y, int w, int h, Color color) {
    if (w <= 0 || h <= 0) return;
    draw_hline(x, y, w, color);
    if (h > 1) draw_hline(x, y + h - 1, w, color);
    if (h > 2) {
        draw_vline(x, y + 1, h - 2, color);
        if (w > 1) draw_vline(x + w - 1, y + 1, h - 2, color);
    }
}

void Canvas::fill_rect(int x, int y, int w, int h, Color color) {
    const Rect r = clipped_rect(x, y, w, h);
    if (r.empty()) return;
    display_.lock();
    display_.fill_rect_unlocked(r.x, r.y, r.w, r.h, color);
    display_.unlock();
}

void Canvas::draw_circle(int cx, int cy, int radius, Color color) {
    if (radius < 0) return;
    display_.lock();
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        set_pixel_unlocked(cx + x, cy + y, color);
        set_pixel_unlocked(cx + y, cy + x, color);
        set_pixel_unlocked(cx - y, cy + x, color);
        set_pixel_unlocked(cx - x, cy + y, color);
        set_pixel_unlocked(cx - x, cy - y, color);
        set_pixel_unlocked(cx - y, cy - x, color);
        set_pixel_unlocked(cx + y, cy - x, color);
        set_pixel_unlocked(cx + x, cy - y, color);
        ++y;
        if (error < 0) error += 2 * y + 1;
        else { --x; error += 2 * (y - x) + 1; }
    }
    display_.unlock();
}

void Canvas::fill_circle(int cx, int cy, int radius, Color color) {
    if (radius < 0) return;
    display_.lock();
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        Rect a = clipped_rect(cx - x, cy + y, 2 * x + 1, 1);
        Rect b = clipped_rect(cx - x, cy - y, 2 * x + 1, 1);
        Rect c = clipped_rect(cx - y, cy + x, 2 * y + 1, 1);
        Rect d = clipped_rect(cx - y, cy - x, 2 * y + 1, 1);
        if (!a.empty()) display_.draw_hline_unlocked(a.x, a.y, a.w, color);
        if (!b.empty()) display_.draw_hline_unlocked(b.x, b.y, b.w, color);
        if (!c.empty()) display_.draw_hline_unlocked(c.x, c.y, c.w, color);
        if (!d.empty()) display_.draw_hline_unlocked(d.x, d.y, d.w, color);
        ++y;
        if (error < 0) error += 2 * y + 1;
        else { --x; error += 2 * (y - x) + 1; }
    }
    display_.unlock();
}

void Canvas::draw_bitmap(int x, int y, const Bitmap& bitmap,
                         Color foreground, bool transparent_background,
                         Color background) {
    if (!bitmap.valid()) return;
    display_.lock();
    for (int row = 0; row < bitmap.height; ++row) {
        for (int col = 0; col < bitmap.width; ++col) {
            const int px = x + col;
            const int py = y + row;
            if (!inside_clip(px, py)) continue;
            if (bitmap.pixel(col, row)) {
                display_.set_pixel_unlocked(px, py, foreground);
            } else if (!transparent_background) {
                display_.set_pixel_unlocked(px, py, background);
            }
        }
    }
    display_.unlock();
}

void Canvas::draw_sprite(const Sprite& sprite) {
    if (!sprite.visible) return;
    draw_bitmap(sprite.x, sprite.y, sprite.bitmap, sprite.foreground,
                sprite.transparent_background, sprite.background);
}

void Canvas::draw_char_unlocked(int x, int y, char character, const Font& font,
                                Color color, int scale,
                                bool transparent_background, Color background) {
    scale = std::max(1, scale);
    for (int column = 0; column < font.glyph_width; ++column) {
        const uint8_t bits = font.glyph_column(character, column);
        for (int row = 0; row < font.glyph_height; ++row) {
            const Color pixel_color = (bits & (1U << row)) ? color : background;
            if (pixel_color == background && transparent_background) continue;
            const int px = x + column * scale;
            const int py = y + row * scale;
            if (scale == 1) set_pixel_unlocked(px, py, pixel_color);
            else {
                const Rect r = clipped_rect(px, py, scale, scale);
                if (!r.empty()) display_.fill_rect_unlocked(r.x, r.y, r.w, r.h, pixel_color);
            }
        }
    }
}

void Canvas::draw_char(int x, int y, char character, const Font& font,
                       Color color, int scale, bool transparent_background,
                       Color background) {
    display_.lock();
    draw_char_unlocked(x, y, character, font, color, scale,
                       transparent_background, background);
    display_.unlock();
}

void Canvas::draw_text(int x, int y, const char* text, const Font& font,
                       Color color, int scale, bool transparent_background,
                       Color background) {
    if (!text || !font.column_fn) return;
    scale = std::max(1, scale);
    display_.lock();
    int cursor_x = x;
    int cursor_y = y;
    while (*text) {
        if (*text == '\n') {
            cursor_y += (font.glyph_height + 1) * scale;
            cursor_x = x;
            ++text;
            continue;
        }
        draw_char_unlocked(cursor_x, cursor_y, *text++, font, color, scale,
                           transparent_background, background);
        cursor_x += (font.glyph_width + font.spacing) * scale;
    }
    display_.unlock();
}

} // namespace pogopo::gfx
