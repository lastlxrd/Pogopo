#include "pogopo_sharp.h"

#include <cstring>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace pogopo {

static const char* TAG = "sharp";
static constexpr spi_host_device_t SHARP_SPI_HOST = SPI2_HOST;

// Sharp Memory LCD protocol bits when SPI is sent LSB-first.
static constexpr uint8_t SHARP_CMD_WRITE = 0x01;
static constexpr uint8_t SHARP_CMD_VCOM  = 0x02;
static constexpr uint8_t SHARP_CMD_CLEAR = 0x04;

SharpDisplay::~SharpDisplay() {
    if (spi_) {
        spi_bus_remove_device(reinterpret_cast<spi_device_handle_t>(spi_));
        spi_ = nullptr;
    }
    if (tx_) {
        heap_caps_free(tx_);
        tx_ = nullptr;
    }
    if (fb_) {
        heap_caps_free(fb_);
        fb_ = nullptr;
    }
    if (mutex_) {
        vSemaphoreDelete(reinterpret_cast<SemaphoreHandle_t>(mutex_));
        mutex_ = nullptr;
    }
}

void SharpDisplay::lock() {
    if (mutex_) xSemaphoreTake(reinterpret_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
}

void SharpDisplay::unlock() {
    if (mutex_) xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_));
}

uint8_t SharpDisplay::command_byte(uint8_t cmd) const {
    return static_cast<uint8_t>(cmd | (vcom_ ? SHARP_CMD_VCOM : 0));
}

esp_err_t SharpDisplay::init() {
    Config cfg;
    return init(cfg);
}

esp_err_t SharpDisplay::init(const Config& cfg) {
    cfg_ = cfg;

    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return ESP_ERR_NO_MEM;

    if (cfg_.disp_io >= 0) {
        gpio_config_t disp_cfg = {};
        disp_cfg.pin_bit_mask = 1ULL << cfg_.disp_io;
        disp_cfg.mode = GPIO_MODE_OUTPUT;
        ESP_ERROR_CHECK(gpio_config(&disp_cfg));
        gpio_set_level(static_cast<gpio_num_t>(cfg_.disp_io), 1);
    }

    if (cfg_.extmode_io >= 0) {
        gpio_config_t ext_cfg = {};
        ext_cfg.pin_bit_mask = 1ULL << cfg_.extmode_io;
        ext_cfg.mode = GPIO_MODE_OUTPUT;
        ESP_ERROR_CHECK(gpio_config(&ext_cfg));
        gpio_set_level(static_cast<gpio_num_t>(cfg_.extmode_io), 0);
    }

    // Sharp Memory LCD uses an ACTIVE-HIGH SCS signal. ESP-IDF hardware CS is
    // active-low by default, so GPIO14 is controlled manually and kept LOW idle.
    gpio_config_t cs_cfg = {};
    cs_cfg.pin_bit_mask = 1ULL << cfg_.cs_io;
    cs_cfg.mode = GPIO_MODE_OUTPUT;
    cs_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cs_cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    cs_cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cs_cfg));
    gpio_set_level(static_cast<gpio_num_t>(cfg_.cs_io), 0);
    esp_rom_delay_us(2);

    fb_ = static_cast<uint8_t*>(heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!fb_) {
        ESP_LOGW(TAG, "PSRAM framebuffer alloc failed, trying internal RAM");
        fb_ = static_cast<uint8_t*>(heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!fb_) {
        ESP_LOGE(TAG, "Framebuffer alloc failed (%u bytes)", static_cast<unsigned>(FRAMEBUFFER_SIZE));
        return ESP_ERR_NO_MEM;
    }

    tx_capacity_ = 1 + HEIGHT * (1 + BYTES_PER_ROW + 1) + 1;
    tx_ = static_cast<uint8_t*>(heap_caps_malloc(tx_capacity_, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (!tx_) {
        ESP_LOGE(TAG, "TX DMA buffer alloc failed (%u bytes)", static_cast<unsigned>(tx_capacity_));
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = cfg_.mosi_io;
    bus_cfg.miso_io_num = -1;
    bus_cfg.sclk_io_num = cfg_.sck_io;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = static_cast<int>(tx_capacity_);

    esp_err_t err = spi_bus_initialize(SHARP_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = cfg_.clock_hz;
    dev_cfg.mode = 0;
    // No automatic CS: the panel requires SCS HIGH during a transaction.
    dev_cfg.spics_io_num = -1;
    dev_cfg.queue_size = 1;
    dev_cfg.flags = SPI_DEVICE_TXBIT_LSBFIRST;

    spi_device_handle_t dev = nullptr;
    err = spi_bus_add_device(SHARP_SPI_HOST, &dev_cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }
    spi_ = dev;

    // From this point the SPI device and buffers exist. Mark the driver alive before
    // calling refresh_full(), because refresh_rows() intentionally rejects calls on
    // a non-initialized display. The previous version set initialized_ only after
    // refresh_full(), so init() always returned ESP_ERR_INVALID_STATE.
    initialized_ = true;

    clear(WHITE);
    err = clear_lcd_hw();
    if (err == ESP_OK) err = refresh_full();

    if (err != ESP_OK) {
        initialized_ = false;
    }

    ESP_LOGI(TAG, "Sharp LS027B7DH01 init %s, fb=%u bytes tx=%u bytes SPI=%d Hz, CS=manual active-HIGH",
             initialized_ ? "OK" : "FAIL",
             static_cast<unsigned>(FRAMEBUFFER_SIZE),
             static_cast<unsigned>(tx_capacity_), cfg_.clock_hz);
    return err;
}

void SharpDisplay::mark_dirty_row(int y) {
    if (y >= 0 && y < HEIGHT) dirty_[y] = true;
}

void SharpDisplay::mark_dirty_range(int y, int h) {
    if (h <= 0) return;
    int y0 = y < 0 ? 0 : y;
    int y1 = y + h;
    if (y1 > HEIGHT) y1 = HEIGHT;
    for (int yy = y0; yy < y1; ++yy) dirty_[yy] = true;
}

void SharpDisplay::clear(Color color) {
    lock();
    std::memset(fb_, color == WHITE ? 0xFF : 0x00, FRAMEBUFFER_SIZE);
    for (bool &d : dirty_) d = true;
    unlock();
}

void SharpDisplay::draw_pixel(int x, int y, Color color) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT || !fb_) return;
    lock();
    uint8_t& b = fb_[y * BYTES_PER_ROW + (x >> 3)];
    const uint8_t mask = static_cast<uint8_t>(1u << (x & 7));
    if (color == WHITE) b |= mask;
    else b &= static_cast<uint8_t>(~mask);
    dirty_[y] = true;
    unlock();
}

void SharpDisplay::draw_hline(int x, int y, int w, Color color) {
    if (y < 0 || y >= HEIGHT || w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > WIDTH) w = WIDTH - x;
    if (w <= 0) return;
    for (int xx = x; xx < x + w; ++xx) draw_pixel(xx, y, color);
}

void SharpDisplay::draw_vline(int x, int y, int h, Color color) {
    if (x < 0 || x >= WIDTH || h <= 0) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > HEIGHT) h = HEIGHT - y;
    if (h <= 0) return;
    for (int yy = y; yy < y + h; ++yy) draw_pixel(x, yy, color);
}

void SharpDisplay::draw_rect(int x, int y, int w, int h, Color color) {
    draw_hline(x, y, w, color);
    draw_hline(x, y + h - 1, w, color);
    draw_vline(x, y, h, color);
    draw_vline(x + w - 1, y, h, color);
}

void SharpDisplay::fill_rect(int x, int y, int w, int h, Color color) {
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; ++yy) draw_hline(x, yy, w, color);
}

static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // space only fallback table starts manually below
};

static uint8_t glyph_col(char c, int col) {
    // Tiny minimal 5x7 font for bring-up: letters/numbers used in diagnostics.
    // Bits are vertical, bit0 is top. Unknown chars become a small box.
    struct G { char c; uint8_t d[5]; };
    static const G g[] = {
        {' ', {0x00,0x00,0x00,0x00,0x00}}, {'-', {0x08,0x08,0x08,0x08,0x08}}, {'.', {0x00,0x00,0x40,0x00,0x00}}, {':', {0x00,0x24,0x00,0x24,0x00}},
        {'0', {0x3E,0x51,0x49,0x45,0x3E}}, {'1', {0x00,0x42,0x7F,0x40,0x00}}, {'2', {0x42,0x61,0x51,0x49,0x46}}, {'3', {0x21,0x41,0x45,0x4B,0x31}}, {'4', {0x18,0x14,0x12,0x7F,0x10}},
        {'5', {0x27,0x45,0x45,0x45,0x39}}, {'6', {0x3C,0x4A,0x49,0x49,0x30}}, {'7', {0x01,0x71,0x09,0x05,0x03}}, {'8', {0x36,0x49,0x49,0x49,0x36}}, {'9', {0x06,0x49,0x49,0x29,0x1E}},
        {'A', {0x7E,0x11,0x11,0x11,0x7E}}, {'B', {0x7F,0x49,0x49,0x49,0x36}}, {'C', {0x3E,0x41,0x41,0x41,0x22}}, {'D', {0x7F,0x41,0x41,0x22,0x1C}}, {'E', {0x7F,0x49,0x49,0x49,0x41}},
        {'F', {0x7F,0x09,0x09,0x09,0x01}}, {'G', {0x3E,0x41,0x49,0x49,0x7A}}, {'H', {0x7F,0x08,0x08,0x08,0x7F}}, {'I', {0x00,0x41,0x7F,0x41,0x00}}, {'K', {0x7F,0x08,0x14,0x22,0x41}},
        {'L', {0x7F,0x40,0x40,0x40,0x40}}, {'M', {0x7F,0x02,0x0C,0x02,0x7F}}, {'N', {0x7F,0x04,0x08,0x10,0x7F}}, {'O', {0x3E,0x41,0x41,0x41,0x3E}}, {'P', {0x7F,0x09,0x09,0x09,0x06}},
        {'R', {0x7F,0x09,0x19,0x29,0x46}}, {'S', {0x46,0x49,0x49,0x49,0x31}}, {'T', {0x01,0x01,0x7F,0x01,0x01}}, {'U', {0x3F,0x40,0x40,0x40,0x3F}}, {'V', {0x1F,0x20,0x40,0x20,0x1F}},
        {'X', {0x63,0x14,0x08,0x14,0x63}}, {'Y', {0x07,0x08,0x70,0x08,0x07}}, {'Z', {0x61,0x51,0x49,0x45,0x43}},
        {'a', {0x20,0x54,0x54,0x54,0x78}}, {'b', {0x7F,0x48,0x44,0x44,0x38}}, {'c', {0x38,0x44,0x44,0x44,0x20}}, {'d', {0x38,0x44,0x44,0x48,0x7F}}, {'e', {0x38,0x54,0x54,0x54,0x18}},
        {'g', {0x08,0x54,0x54,0x54,0x3C}}, {'h', {0x7F,0x08,0x04,0x04,0x78}}, {'i', {0x00,0x44,0x7D,0x40,0x00}}, {'l', {0x00,0x41,0x7F,0x40,0x00}}, {'m', {0x7C,0x04,0x18,0x04,0x78}},
        {'n', {0x7C,0x08,0x04,0x04,0x78}}, {'o', {0x38,0x44,0x44,0x44,0x38}}, {'p', {0x7C,0x14,0x14,0x14,0x08}}, {'r', {0x7C,0x08,0x04,0x04,0x08}}, {'s', {0x48,0x54,0x54,0x54,0x20}},
        {'t', {0x04,0x3F,0x44,0x40,0x20}}, {'u', {0x3C,0x40,0x40,0x20,0x7C}}, {'v', {0x1C,0x20,0x40,0x20,0x1C}}, {'x', {0x44,0x28,0x10,0x28,0x44}}, {'y', {0x0C,0x50,0x50,0x50,0x3C}},
    };
    for (const auto& it : g) if (it.c == c) return it.d[col];
    return (col == 0 || col == 4) ? 0x7F : 0x41;
}

void SharpDisplay::draw_char_5x7(int x, int y, char c, Color color, int scale) {
    if (scale < 1) scale = 1;
    for (int cx = 0; cx < 5; ++cx) {
        uint8_t bits = glyph_col(c, cx);
        for (int cy = 0; cy < 7; ++cy) {
            if (bits & (1u << cy)) {
                if (scale == 1) draw_pixel(x + cx, y + cy, color);
                else fill_rect(x + cx * scale, y + cy * scale, scale, scale, color);
            }
        }
    }
}

void SharpDisplay::draw_text(int x, int y, const char* text, Color color, int scale) {
    if (!text) return;
    int xx = x;
    while (*text) {
        if (*text == '\n') { y += 8 * scale; xx = x; ++text; continue; }
        draw_char_5x7(xx, y, *text++, color, scale);
        xx += 6 * scale;
    }
}

esp_err_t SharpDisplay::transmit_bytes(const uint8_t* data, size_t len) {
    if (!spi_ || !data || len == 0) return ESP_ERR_INVALID_STATE;

    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = data;

    // LS027B7DH01 timing: SCS must rise before the first clock and remain HIGH
    // for the entire command/data/trailer sequence. Keep explicit margins here
    // during bring-up; we can optimize them later.
    gpio_set_level(static_cast<gpio_num_t>(cfg_.cs_io), 1);
    esp_rom_delay_us(4);  // datasheet tsSCS >= 3 us

    esp_err_t err = spi_device_transmit(
        reinterpret_cast<spi_device_handle_t>(spi_), &t);

    esp_rom_delay_us(2);  // datasheet thSCS >= 1 us
    gpio_set_level(static_cast<gpio_num_t>(cfg_.cs_io), 0);
    esp_rom_delay_us(2);  // datasheet twSCSL >= 1 us
    return err;
}

esp_err_t SharpDisplay::clear_lcd_hw() {
    lock();
    vcom_ = !vcom_;
    uint8_t cmd[2] = { command_byte(SHARP_CMD_CLEAR), 0x00 };
    esp_err_t err = transmit_bytes(cmd, sizeof(cmd));
    unlock();
    return err;
}

esp_err_t SharpDisplay::toggle_vcom_only() {
    lock();
    vcom_ = !vcom_;
    uint8_t cmd[2] = { command_byte(0x00), 0x00 };
    esp_err_t err = transmit_bytes(cmd, sizeof(cmd));
    unlock();
    return err;
}

esp_err_t SharpDisplay::refresh_rows(int y, int h) {
    if (!initialized_ || !fb_) return ESP_ERR_INVALID_STATE;
    if (h <= 0) return ESP_OK;
    int y0 = y < 0 ? 0 : y;
    int y1 = y + h;
    if (y1 > HEIGHT) y1 = HEIGHT;
    if (y0 >= y1) return ESP_OK;
    const int rows = y1 - y0;
    const size_t needed = 1 + rows * (1 + BYTES_PER_ROW + 1) + 1;
    if (needed > tx_capacity_) return ESP_ERR_NO_MEM;

    lock();
    vcom_ = !vcom_;
    size_t p = 0;
    tx_[p++] = command_byte(SHARP_CMD_WRITE);
    for (int yy = y0; yy < y1; ++yy) {
        tx_[p++] = static_cast<uint8_t>(yy + 1); // line number 1..240, LSB-first SPI
        std::memcpy(&tx_[p], &fb_[yy * BYTES_PER_ROW], BYTES_PER_ROW);
        p += BYTES_PER_ROW;
        tx_[p++] = 0x00; // line trailer
        dirty_[yy] = false;
    }
    tx_[p++] = 0x00; // final trailer
    esp_err_t err = transmit_bytes(tx_, p);
    unlock();
    return err;
}

esp_err_t SharpDisplay::refresh_full() {
    return refresh_rows(0, HEIGHT);
}

esp_err_t SharpDisplay::refresh_dirty() {
    if (!initialized_) return ESP_ERR_INVALID_STATE;

    int y = 0;
    esp_err_t final_err = ESP_OK;
    while (y < HEIGHT) {
        while (y < HEIGHT && !dirty_[y]) ++y;
        if (y >= HEIGHT) break;
        int start = y;
        while (y < HEIGHT && dirty_[y]) ++y;
        esp_err_t err = refresh_rows(start, y - start);
        if (err != ESP_OK) final_err = err;
    }
    return final_err;
}

} // namespace pogopo
