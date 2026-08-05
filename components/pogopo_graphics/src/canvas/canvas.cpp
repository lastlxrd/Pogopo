#include "pogopo/gfx/canvas.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

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

void Canvas::draw_indexed2_scaled(int x, int y, int source_width, int source_height,
                                  const uint8_t* pixels, int destination_width,
                                  int destination_height,
                                  const uint8_t* previous_pixels,
                                  bool dither, bool invert) {
    if (!pixels || source_width <= 0 || source_height <= 0 ||
        destination_width <= 0 || destination_height <= 0) {
        return;
    }

    static constexpr uint8_t bayer2x2[4] = {0, 2, 3, 1};
    static constexpr uint8_t black_levels[4] = {0, 1, 3, 4};

    display_.lock();
    for (int sy = 0; sy < source_height; ++sy) {
        const int y0 = y + (sy * destination_height) / source_height;
        const int y1 = y + ((sy + 1) * destination_height) / source_height;
        if (y1 <= clip_.y || y0 >= clip_.bottom()) continue;

        for (int sx = 0; sx < source_width; ++sx) {
            const int source_index = sy * source_width + sx;
            const uint8_t original = static_cast<uint8_t>(pixels[source_index] & 0x03U);
            if (previous_pixels &&
                original == static_cast<uint8_t>(previous_pixels[source_index] & 0x03U)) {
                continue;
            }

            const uint8_t shade = invert ? static_cast<uint8_t>(3U - original) : original;
            const int x0 = x + (sx * destination_width) / source_width;
            const int x1 = x + ((sx + 1) * destination_width) / source_width;
            const Rect destination = clipped_rect(x0, y0,
                                                   std::max(1, x1 - x0),
                                                   std::max(1, y1 - y0));
            if (destination.empty()) continue;

            if (!dither || shade == 0 || shade == 3) {
                const bool black = shade == 3 || (!dither && shade >= 2);
                display_.fill_rect_unlocked(destination.x, destination.y,
                                            destination.w, destination.h,
                                            black ? BLACK : WHITE);
                continue;
            }

            const uint8_t level = black_levels[shade];
            for (int py = destination.y; py < destination.bottom(); ++py) {
                for (int px = destination.x; px < destination.right(); ++px) {
                    const uint8_t threshold = bayer2x2[((py & 1) << 1) | (px & 1)];
                    display_.set_pixel_unlocked(px, py,
                                                threshold < level ? BLACK : WHITE);
                }
            }
        }
    }
    display_.unlock();
}

void Canvas::draw_indexed2_fast(int x, int y, int source_width, int source_height,
                                const uint8_t* pixels, int destination_width,
                                int destination_height, bool dither, bool invert) {
    if (!pixels || source_width <= 0 || source_height <= 0 ||
        destination_width <= 0 || destination_height <= 0) {
        return;
    }

    // Celeste's fullscreen mode is exactly 200x120 -> 400x240, monochrome,
    // and covers the complete Sharp panel.  The generic scaler below visits
    // all 96,000 destination pixels and updates one bit at a time.  Pack four
    // logical pixels straight into one Sharp byte, then duplicate the finished
    // row.  Clear and white are both white on the final opaque framebuffer.
    const bool native_mono_2x = !dither && !invert && x == 0 && y == 0 &&
        source_width == 200 && source_height == 120 &&
        destination_width == SharpDisplay::WIDTH &&
        destination_height == SharpDisplay::HEIGHT &&
        clip_.x <= 0 && clip_.y <= 0 &&
        clip_.right() >= SharpDisplay::WIDTH &&
        clip_.bottom() >= SharpDisplay::HEIGHT;
    if (native_mono_2x) {
        static constexpr uint8_t white_pair[4] = {
            0x03U, 0x03U, 0x00U, 0x00U,
        };
        display_.lock();
        for (int sy = 0; sy < source_height; ++sy) {
            const uint8_t* source_row = pixels +
                static_cast<size_t>(sy) * source_width;
            uint8_t packed[SharpDisplay::BYTES_PER_ROW];
            for (int group = 0; group < SharpDisplay::BYTES_PER_ROW; ++group) {
                const uint8_t* source = source_row + group * 4;
                packed[group] = static_cast<uint8_t>(
                    white_pair[source[0] & 0x03U] |
                    (white_pair[source[1] & 0x03U] << 2U) |
                    (white_pair[source[2] & 0x03U] << 4U) |
                    (white_pair[source[3] & 0x03U] << 6U));
            }
            for (int repeat = 0; repeat < 2; ++repeat) {
                const int py = sy * 2 + repeat;
                uint8_t* framebuffer_row = display_.fb_ +
                    static_cast<size_t>(py) * SharpDisplay::BYTES_PER_ROW;
                if (std::memcmp(framebuffer_row, packed, sizeof(packed)) != 0) {
                    std::memcpy(framebuffer_row, packed, sizeof(packed));
                    display_.mark_dirty_row_unlocked(py);
                }
            }
        }
        display_.unlock();
        return;
    }

    static constexpr uint8_t bayer2x2[4] = {0, 2, 3, 1};
    static constexpr uint8_t black_levels[4] = {0, 1, 3, 4};
    uint16_t source_x[SharpDisplay::WIDTH]{};
    const int clipped_width = std::min(destination_width, SharpDisplay::WIDTH);
    for (int dx = 0; dx < clipped_width; ++dx) {
        source_x[dx] = static_cast<uint16_t>(
            (static_cast<int64_t>(dx) * source_width) / destination_width);
    }

    display_.lock();
    for (int dy = 0; dy < destination_height; ++dy) {
        const int py = y + dy;
        if (py < clip_.y || py >= clip_.bottom() ||
            py < 0 || py >= SharpDisplay::HEIGHT) {
            continue;
        }
        const int sy = static_cast<int>(
            (static_cast<int64_t>(dy) * source_height) / destination_height);
        const uint8_t* source_row = pixels + static_cast<size_t>(sy) * source_width;
        uint8_t* framebuffer_row = display_.fb_ +
            static_cast<size_t>(py) * SharpDisplay::BYTES_PER_ROW;
        uint8_t packed[SharpDisplay::BYTES_PER_ROW];
        std::memcpy(packed, framebuffer_row, sizeof(packed));

        for (int dx = 0; dx < clipped_width; ++dx) {
            const int px = x + dx;
            if (px < clip_.x || px >= clip_.right() ||
                px < 0 || px >= SharpDisplay::WIDTH) {
                continue;
            }
            const uint8_t original = static_cast<uint8_t>(source_row[source_x[dx]] & 0x03U);
            const uint8_t shade = invert ? static_cast<uint8_t>(3U - original) : original;
            bool black;
            if (!dither || shade == 0 || shade == 3) {
                black = shade == 3 || (!dither && shade >= 2);
            } else {
                const uint8_t threshold = bayer2x2[((py & 1) << 1) | (px & 1)];
                black = threshold < black_levels[shade];
            }
            const uint8_t mask = static_cast<uint8_t>(1U << (px & 7));
            if (black) packed[px >> 3] &= static_cast<uint8_t>(~mask);
            else packed[px >> 3] |= mask;
        }

        if (std::memcmp(framebuffer_row, packed, sizeof(packed)) != 0) {
            std::memcpy(framebuffer_row, packed, sizeof(packed));
            display_.mark_dirty_row_unlocked(py);
        }
    }
    display_.unlock();
}

void Canvas::draw_indexed2_packed_fast(int x, int y, int source_width, int source_height,
                                       const uint8_t* pixels, int destination_width,
                                       int destination_height, bool dither, bool invert) {
    if (!pixels || source_width <= 0 || source_height <= 0 ||
        destination_width <= 0 || destination_height <= 0) {
        return;
    }

    static constexpr uint8_t bayer2x2[4] = {0, 2, 3, 1};
    static constexpr uint8_t black_levels[4] = {0, 1, 3, 4};
    const size_t source_stride = static_cast<size_t>(source_width + 3) / 4U;

    // Build the complete shade/parity decision table once per frame instead
    // of branching for every one of the 64k scaled destination pixels.
    bool black_lut[16]{};
    for (uint8_t original = 0; original < 4; ++original) {
        const uint8_t shade = invert ? static_cast<uint8_t>(3U - original) : original;
        for (uint8_t parity = 0; parity < 4; ++parity) {
            black_lut[(original << 2U) | parity] = !dither
                ? shade >= 2
                : black_levels[shade] > bayer2x2[parity];
        }
    }

    uint16_t source_x[SharpDisplay::WIDTH]{};
    const int clipped_width = std::min(destination_width, SharpDisplay::WIDTH);
    int mapped_x = 0;
    int x_error = 0;
    for (int dx = 0; dx < clipped_width; ++dx) {
        source_x[dx] = static_cast<uint16_t>(
            std::min(mapped_x, source_width - 1));
        x_error += source_width;
        while (x_error >= destination_width) {
            x_error -= destination_width;
            ++mapped_x;
        }
    }

    // Game Boy FitHeight is fully on-screen (66,0,267,240). Its old generic
    // path performed four clipping comparisons for every pixel even though
    // none could ever be clipped.
    const bool fully_visible = x >= clip_.x && y >= clip_.y &&
        x + destination_width <= clip_.right() &&
        y + destination_height <= clip_.bottom() && x >= 0 && y >= 0 &&
        x + destination_width <= SharpDisplay::WIDTH &&
        y + destination_height <= SharpDisplay::HEIGHT &&
        destination_width <= SharpDisplay::WIDTH;

    display_.lock();
    int mapped_y = 0;
    int y_error = 0;
    for (int dy = 0; dy < destination_height; ++dy) {
        const int py = y + dy;
        if (!fully_visible && (py < clip_.y || py >= clip_.bottom() ||
            py < 0 || py >= SharpDisplay::HEIGHT)) {
            continue;
        }
        const int sy = fully_visible ? mapped_y : static_cast<int>(
            (static_cast<int64_t>(dy) * source_height) / destination_height);
        const uint8_t* source_row = pixels + static_cast<size_t>(sy) * source_stride;
        uint8_t* framebuffer_row = display_.fb_ +
            static_cast<size_t>(py) * SharpDisplay::BYTES_PER_ROW;
        uint8_t packed[SharpDisplay::BYTES_PER_ROW];
        std::memcpy(packed, framebuffer_row, sizeof(packed));

        if (fully_visible) {
            for (int dx = 0; dx < destination_width; ++dx) {
                const int px = x + dx;
                const uint16_t sx = source_x[dx];
                const uint8_t original = static_cast<uint8_t>(
                    (source_row[sx >> 2U] >> ((sx & 0x03U) * 2U)) & 0x03U);
                const uint8_t parity = static_cast<uint8_t>(
                    ((py & 1) << 1) | (px & 1));
                const uint8_t mask = static_cast<uint8_t>(1U << (px & 7));
                if (black_lut[(original << 2U) | parity]) {
                    packed[px >> 3] &= static_cast<uint8_t>(~mask);
                } else {
                    packed[px >> 3] |= mask;
                }
            }
        } else {
            for (int dx = 0; dx < clipped_width; ++dx) {
                const int px = x + dx;
                if (px < clip_.x || px >= clip_.right() ||
                    px < 0 || px >= SharpDisplay::WIDTH) {
                    continue;
                }
                const uint16_t sx = source_x[dx];
                const uint8_t original = static_cast<uint8_t>(
                    (source_row[sx >> 2U] >> ((sx & 0x03U) * 2U)) & 0x03U);
                const uint8_t parity = static_cast<uint8_t>(
                    ((py & 1) << 1) | (px & 1));
                const uint8_t mask = static_cast<uint8_t>(1U << (px & 7));
                if (black_lut[(original << 2U) | parity]) {
                    packed[px >> 3] &= static_cast<uint8_t>(~mask);
                } else {
                    packed[px >> 3] |= mask;
                }
            }
        }

        if (std::memcmp(framebuffer_row, packed, sizeof(packed)) != 0) {
            std::memcpy(framebuffer_row, packed, sizeof(packed));
            display_.mark_dirty_row_unlocked(py);
        }

        if (fully_visible) {
            y_error += source_height;
            while (y_error >= destination_height) {
                y_error -= destination_height;
                ++mapped_y;
            }
            mapped_y = std::min(mapped_y, source_height - 1);
        }
    }
    display_.unlock();
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
