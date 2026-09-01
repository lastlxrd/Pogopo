#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "pogopo/gfx/types.h"

namespace pogopo::gfx {

class Canvas;

class SharpDisplay {
public:
    static constexpr int WIDTH = 400;
    static constexpr int HEIGHT = 240;
    static constexpr int BYTES_PER_ROW = WIDTH / 8;
    static constexpr size_t FRAMEBUFFER_SIZE = BYTES_PER_ROW * HEIGHT;

    using Color = ::pogopo::gfx::Color;
    static constexpr Color BLACK = Color::BLACK;
    static constexpr Color WHITE = Color::WHITE;

    struct Config {
        int sck_io = 12;
        int mosi_io = 11;
        int cs_io = 14;
        int disp_io = -1;
        int extmode_io = -1;
        // Proven stable by the pre-IDF Arduino firmware on this exact panel
        // and wiring. 2 MHz capped full-frame updates near 20 FPS.
        int clock_hz = 14000000;

        bool enable_vcom_task = true;
        uint32_t vcom_period_ms = 500;
        uint32_t vcom_task_stack = 2048;
        unsigned vcom_task_priority = 4;
        int vcom_task_core = -1;
    };

    struct Stats {
        uint32_t last_refresh_us = 0;
        uint16_t last_rows = 0;
        uint32_t last_bytes = 0;
        uint64_t total_refreshes = 0;
        uint64_t full_refreshes = 0;
        uint64_t partial_refreshes = 0;
        uint64_t total_rows = 0;
        uint64_t total_bytes = 0;
        uint64_t vcom_toggles = 0;
    };

    SharpDisplay() = default;
    ~SharpDisplay();
    SharpDisplay(const SharpDisplay&) = delete;
    SharpDisplay& operator=(const SharpDisplay&) = delete;

    esp_err_t init();
    esp_err_t init(const Config& cfg);
    void deinit();

    bool ok() const { return initialized_; }
    int width() const { return WIDTH; }
    int height() const { return HEIGHT; }

    // Low-level compatibility drawing API. New code should normally use Canvas/Graphics.
    void clear(Color color = WHITE);
    void draw_pixel(int x, int y, Color color);
    void draw_line(int x0, int y0, int x1, int y1, Color color);
    void draw_hline(int x, int y, int w, Color color);
    void draw_vline(int x, int y, int h, Color color);
    void draw_rect(int x, int y, int w, int h, Color color);
    void fill_rect(int x, int y, int w, int h, Color color);
    void draw_circle(int cx, int cy, int radius, Color color);
    void fill_circle(int cx, int cy, int radius, Color color);
    void draw_bitmap_1bpp(int x, int y, int w, int h,
                          const uint8_t* bitmap, Color foreground,
                          bool transparent_background = true,
                          Color background = WHITE);
    void draw_char_5x7(int x, int y, char c, Color color, int scale = 1);
    void draw_text(int x, int y, const char* text, Color color = BLACK, int scale = 1);

    esp_err_t refresh_full();
    esp_err_t refresh_rows(int y, int h);
    esp_err_t refresh_dirty();
    esp_err_t clear_lcd_hw();
    esp_err_t toggle_vcom_only();

    // Replace the complete native framebuffer atomically. Data must be
    // WIDTH x HEIGHT, row-packed LSB-first, with 1=white and 0=black.
    esp_err_t load_framebuffer(const uint8_t* data, size_t size);

    uint8_t* framebuffer() { return fb_; }
    const uint8_t* framebuffer() const { return fb_; }
    size_t framebuffer_size() const { return FRAMEBUFFER_SIZE; }

    Stats get_stats();
    void reset_stats();
    uint16_t dirty_row_count();

private:
    friend class Canvas;

    void lock();
    void unlock();
    void set_pixel_unlocked(int x, int y, Color color);
    void draw_hline_unlocked(int x, int y, int w, Color color);
    void fill_rect_unlocked(int x, int y, int w, int h, Color color);
    void draw_char_unlocked(int x, int y, char c, Color color, int scale);
    void mark_dirty_row_unlocked(int y);
    void mark_dirty_range_unlocked(int y, int h);

    uint8_t command_byte(uint8_t cmd) const;
    esp_err_t transmit_bytes(const uint8_t* data, size_t len);
    void record_refresh_unlocked(uint16_t rows, uint32_t bytes,
                                 uint32_t elapsed_us, bool full);
    esp_err_t start_vcom_task();
    void stop_vcom_task();
    static void vcom_task_entry(void* arg);

    Config cfg_{};
    bool initialized_ = false;
    bool owns_spi_bus_ = false;
    bool vcom_ = false;
    volatile bool vcom_task_stop_ = false;

    void* spi_ = nullptr;
    void* mutex_ = nullptr;
    void* vcom_task_ = nullptr;
    uint8_t* fb_ = nullptr;
    uint8_t* tx_ = nullptr;
    size_t tx_capacity_ = 0;
    bool dirty_[HEIGHT] = {};
    Stats stats_{};
};

} // namespace pogopo::gfx
