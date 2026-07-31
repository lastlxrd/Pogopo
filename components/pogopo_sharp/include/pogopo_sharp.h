#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace pogopo {

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
        int disp_io = -1;      // -1 якщо DISP підтягнутий апаратно
        int extmode_io = -1;   // -1 якщо EXTMODE апаратно посаджений low
        int clock_hz = 2000000;
    };

    SharpDisplay() = default;
    ~SharpDisplay();

    esp_err_t init();
    esp_err_t init(const Config& cfg);
    bool ok() const { return initialized_; }

    void clear(Color color = WHITE);
    void draw_pixel(int x, int y, Color color);
    void draw_hline(int x, int y, int w, Color color);
    void draw_vline(int x, int y, int h, Color color);
    void draw_rect(int x, int y, int w, int h, Color color);
    void fill_rect(int x, int y, int w, int h, Color color);
    void draw_char_5x7(int x, int y, char c, Color color, int scale = 1);
    void draw_text(int x, int y, const char* text, Color color = BLACK, int scale = 1);

    esp_err_t refresh_full();
    esp_err_t refresh_rows(int y, int h);
    esp_err_t refresh_dirty();
    esp_err_t clear_lcd_hw();
    esp_err_t toggle_vcom_only();

    uint8_t* framebuffer() { return fb_; }
    const uint8_t* framebuffer() const { return fb_; }
    size_t framebuffer_size() const { return FRAMEBUFFER_SIZE; }

private:
    void mark_dirty_row(int y);
    void mark_dirty_range(int y, int h);
    void lock();
    void unlock();
    uint8_t command_byte(uint8_t cmd) const;
    esp_err_t transmit_bytes(const uint8_t* data, size_t len);

    Config cfg_{};
    bool initialized_ = false;
    bool vcom_ = false;
    void* spi_ = nullptr;      // spi_device_handle_t, сховано щоб header був чистіший
    void* mutex_ = nullptr;    // SemaphoreHandle_t
    uint8_t* fb_ = nullptr;    // PSRAM framebuffer
    uint8_t* tx_ = nullptr;    // DMA-capable transfer buffer for full/partial refresh
    size_t tx_capacity_ = 0;
    bool dirty_[HEIGHT] = {};
};

} // namespace pogopo
