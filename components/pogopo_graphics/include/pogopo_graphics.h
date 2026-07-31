#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace pogopo {

class Canvas;

class SharpDisplay {
public:
    static constexpr int WIDTH = 400;
    static constexpr int HEIGHT = 240;
    static constexpr int BYTES_PER_ROW = WIDTH / 8;
    static constexpr size_t FRAMEBUFFER_SIZE = BYTES_PER_ROW * HEIGHT;

    enum Color : uint8_t {
        BLACK = 0,
        WHITE = 1,
    };

    struct Config {
        int sck_io = 12;
        int mosi_io = 11;
        int cs_io = 14;
        int disp_io = -1;       // -1: DISP is tied HIGH on the PCB
        int extmode_io = -1;    // -1: EXTMODE is tied LOW on the PCB
        int clock_hz = 2000000; // datasheet-safe bring-up speed

        bool enable_vcom_task = true;
        uint32_t vcom_period_ms = 500;
        uint32_t vcom_task_stack = 2048;
        unsigned vcom_task_priority = 4;
        int vcom_task_core = -1; // -1 = no affinity
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

    // Sharp LCD updates at row granularity. refresh_region() therefore uses y/h;
    // x/w are accepted by Canvas for a conventional graphics API but do not
    // reduce the SPI payload further than complete rows.
    esp_err_t refresh_full();
    esp_err_t refresh_rows(int y, int h);
    esp_err_t refresh_dirty();
    esp_err_t clear_lcd_hw();
    esp_err_t toggle_vcom_only();

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

    void* spi_ = nullptr;       // spi_device_handle_t
    void* mutex_ = nullptr;     // SemaphoreHandle_t
    void* vcom_task_ = nullptr; // TaskHandle_t

    uint8_t* fb_ = nullptr;     // PSRAM framebuffer
    uint8_t* tx_ = nullptr;     // DMA-capable transfer buffer
    size_t tx_capacity_ = 0;
    bool dirty_[HEIGHT] = {};
    Stats stats_{};
};

class Canvas {
public:
    using Color = SharpDisplay::Color;
    static constexpr Color BLACK = SharpDisplay::BLACK;
    static constexpr Color WHITE = SharpDisplay::WHITE;

    explicit Canvas(SharpDisplay& display) : display_(display) {}

    int width() const { return display_.width(); }
    int height() const { return display_.height(); }

    void clear(Color color = WHITE) { display_.clear(color); }
    void pixel(int x, int y, Color color) { display_.draw_pixel(x, y, color); }
    void line(int x0, int y0, int x1, int y1, Color color) {
        display_.draw_line(x0, y0, x1, y1, color);
    }
    void hline(int x, int y, int w, Color color) { display_.draw_hline(x, y, w, color); }
    void vline(int x, int y, int h, Color color) { display_.draw_vline(x, y, h, color); }
    void rect(int x, int y, int w, int h, Color color) {
        display_.draw_rect(x, y, w, h, color);
    }
    void fill_rect(int x, int y, int w, int h, Color color) {
        display_.fill_rect(x, y, w, h, color);
    }
    void circle(int x, int y, int r, Color color) { display_.draw_circle(x, y, r, color); }
    void fill_circle(int x, int y, int r, Color color) {
        display_.fill_circle(x, y, r, color);
    }
    void bitmap_1bpp(int x, int y, int w, int h, const uint8_t* data,
                     Color foreground = BLACK,
                     bool transparent_background = true,
                     Color background = WHITE) {
        display_.draw_bitmap_1bpp(x, y, w, h, data, foreground,
                                  transparent_background, background);
    }
    void text(int x, int y, const char* value, Color color = BLACK, int scale = 1) {
        display_.draw_text(x, y, value, color, scale);
    }

    esp_err_t present() { return display_.refresh_dirty(); }
    esp_err_t present_full() { return display_.refresh_full(); }
    esp_err_t present_region(int x, int y, int w, int h) {
        (void)x;
        (void)w;
        return display_.refresh_rows(y, h);
    }

    SharpDisplay& display() { return display_; }

private:
    SharpDisplay& display_;
};

} // namespace pogopo
