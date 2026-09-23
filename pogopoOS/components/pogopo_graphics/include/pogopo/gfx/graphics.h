#pragma once

#include "pogopo/gfx/canvas.h"

namespace pogopo::gfx {

class Graphics {
public:
    using Config = SharpDisplay::Config;
    using Stats = SharpDisplay::Stats;
    static constexpr Color BLACK = Color::BLACK;
    static constexpr Color WHITE = Color::WHITE;

    Graphics();
    Graphics(const Graphics&) = delete;
    Graphics& operator=(const Graphics&) = delete;

    esp_err_t begin();
    esp_err_t begin(const Config& config);
    void end();
    bool ok() const { return display_.ok(); }

    int width() const { return display_.width(); }
    int height() const { return display_.height(); }

    Canvas& canvas() { return canvas_; }
    const Canvas& canvas() const { return canvas_; }
    SharpDisplay& display() { return display_; }
    const SharpDisplay& display() const { return display_; }

    void set_clip(const Rect& rect) { canvas_.set_clip(rect); }
    void reset_clip() { canvas_.reset_clip(); }
    Rect clip() const { return canvas_.clip(); }

    void clear(Color c = WHITE) { canvas_.clear(c); }
    void clear_clip(Color c = WHITE) { canvas_.clear_clip(c); }
    void drawPixel(int x, int y, Color c) { canvas_.draw_pixel(x, y, c); }
    void drawLine(int x0, int y0, int x1, int y1, Color c) { canvas_.draw_line(x0, y0, x1, y1, c); }
    void drawHLine(int x, int y, int w, Color c) { canvas_.draw_hline(x, y, w, c); }
    void drawVLine(int x, int y, int h, Color c) { canvas_.draw_vline(x, y, h, c); }
    void drawRect(int x, int y, int w, int h, Color c) { canvas_.draw_rect(x, y, w, h, c); }
    void fillRect(int x, int y, int w, int h, Color c) { canvas_.fill_rect(x, y, w, h, c); }
    void drawCircle(int x, int y, int r, Color c) { canvas_.draw_circle(x, y, r, c); }
    void fillCircle(int x, int y, int r, Color c) { canvas_.fill_circle(x, y, r, c); }
    void drawBitmap(int x, int y, const Bitmap& bitmap,
                    Color fg = BLACK, bool transparent = true,
                    Color bg = WHITE) {
        canvas_.draw_bitmap(x, y, bitmap, fg, transparent, bg);
    }
    void drawSprite(const Sprite& sprite) { canvas_.draw_sprite(sprite); }
    void drawText(int x, int y, const char* text, Color c = BLACK, int scale = 1) {
        canvas_.draw_text(x, y, text, font5x7(), c, scale);
    }
    void drawText(int x, int y, const char* text, const Font& font,
                  Color c = BLACK, int scale = 1,
                  bool transparent = true, Color bg = WHITE) {
        canvas_.draw_text(x, y, text, font, c, scale, transparent, bg);
    }

    esp_err_t present() { return canvas_.present(); }
    esp_err_t presentFull() { return canvas_.present_full(); }
    esp_err_t presentRegion(const Rect& rect) { return canvas_.present_region(rect); }

    Stats stats() { return display_.get_stats(); }
    void resetStats() { display_.reset_stats(); }
    uint16_t dirtyRows() { return display_.dirty_row_count(); }

private:
    SharpDisplay display_{};
    Canvas canvas_;
};

} // namespace pogopo::gfx

