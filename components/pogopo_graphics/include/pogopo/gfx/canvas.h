#pragma once

#include "esp_err.h"
#include "pogopo/gfx/bitmap.h"
#include "pogopo/gfx/font.h"
#include "pogopo/gfx/sharp_display.h"
#include "pogopo/gfx/sprite.h"
#include "pogopo/gfx/types.h"

namespace pogopo::gfx {

class Canvas {
public:
    explicit Canvas(SharpDisplay& display);

    int width() const { return display_.width(); }
    int height() const { return display_.height(); }

    void set_clip(const Rect& rect);
    void reset_clip();
    Rect clip() const { return clip_; }

    void clear(Color color = WHITE);
    void clear_clip(Color color = WHITE);
    void draw_pixel(int x, int y, Color color);
    void draw_line(int x0, int y0, int x1, int y1, Color color);
    void draw_hline(int x, int y, int w, Color color);
    void draw_vline(int x, int y, int h, Color color);
    void draw_rect(int x, int y, int w, int h, Color color);
    void fill_rect(int x, int y, int w, int h, Color color);
    void draw_circle(int cx, int cy, int radius, Color color);
    void fill_circle(int cx, int cy, int radius, Color color);
    void draw_bitmap(int x, int y, const Bitmap& bitmap,
                     Color foreground = BLACK,
                     bool transparent_background = true,
                     Color background = WHITE);
    void draw_sprite(const Sprite& sprite);
    // Draw a 2-bit indexed image (0=white, 3=black) using nearest-neighbour
    // scaling and an optional 2x2 ordered dither for the two middle shades.
    // When previous_pixels is supplied, unchanged source pixels are skipped.
    void draw_indexed2_scaled(int x, int y, int source_width, int source_height,
                              const uint8_t* pixels, int destination_width,
                              int destination_height,
                              const uint8_t* previous_pixels = nullptr,
                              bool dither = true, bool invert = false);
    // Fast row-packed path for full-frame indexed images such as Game Boy.
    // It builds each destination row directly in the Sharp 1-bpp framebuffer,
    // compares whole rows, and marks only changed rows dirty.
    void draw_indexed2_fast(int x, int y, int source_width, int source_height,
                            const uint8_t* pixels, int destination_width,
                            int destination_height, bool dither = true,
                            bool invert = false);
    // Same renderer for four 2-bit indices packed into each source byte.
    // Pixel 0 occupies bits 1:0, pixel 1 bits 3:2, and so on.
    void draw_indexed2_packed_fast(int x, int y, int source_width, int source_height,
                                   const uint8_t* pixels, int destination_width,
                                   int destination_height, bool dither = true,
                                   bool invert = false);
    // Convert an RGB565 frame directly into the Sharp 1-bpp framebuffer.
    // A 4x4 ordered dither preserves sixteen luminance steps and is intended
    // for colour-console frontends such as the experimental GBA port.
    void draw_rgb565_fast(int x, int y, int source_width, int source_height,
                          const uint16_t* pixels, int destination_width,
                          int destination_height, bool dither = true);
    void draw_char(int x, int y, char character,
                   const Font& font = font5x7(), Color color = BLACK,
                   int scale = 1, bool transparent_background = true,
                   Color background = WHITE);
    void draw_text(int x, int y, const char* text,
                   const Font& font = font5x7(), Color color = BLACK,
                   int scale = 1, bool transparent_background = true,
                   Color background = WHITE);

    esp_err_t present() { return display_.refresh_dirty(); }
    esp_err_t present_full() { return display_.refresh_full(); }
    esp_err_t present_region(const Rect& rect) {
        return display_.refresh_rows(rect.y, rect.h);
    }

    SharpDisplay& display() { return display_; }
    const SharpDisplay& display() const { return display_; }

    // Compact aliases for game/app code.
    void pixel(int x, int y, Color c) { draw_pixel(x, y, c); }
    void line(int x0, int y0, int x1, int y1, Color c) { draw_line(x0, y0, x1, y1, c); }
    void hline(int x, int y, int w, Color c) { draw_hline(x, y, w, c); }
    void vline(int x, int y, int h, Color c) { draw_vline(x, y, h, c); }
    void rect(int x, int y, int w, int h, Color c) { draw_rect(x, y, w, h, c); }
    void fillRect(int x, int y, int w, int h, Color c) { fill_rect(x, y, w, h, c); }
    void circle(int x, int y, int r, Color c) { draw_circle(x, y, r, c); }
    void fillCircle(int x, int y, int r, Color c) { fill_circle(x, y, r, c); }
    void text(int x, int y, const char* value, Color c = BLACK, int scale = 1) {
        draw_text(x, y, value, font5x7(), c, scale);
    }

private:
    bool inside_clip(int x, int y) const { return clip_.contains(x, y); }
    Rect clipped_rect(int x, int y, int w, int h) const;
    void set_pixel_unlocked(int x, int y, Color color);
    void draw_char_unlocked(int x, int y, char character, const Font& font,
                            Color color, int scale,
                            bool transparent_background, Color background);

    SharpDisplay& display_;
    Rect clip_{};
};

} // namespace pogopo::gfx
