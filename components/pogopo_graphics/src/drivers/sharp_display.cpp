#include "pogopo/gfx/sharp_display.h"
#include "pogopo/gfx/font.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace pogopo::gfx {

static const char* TAG = "graphics";
static constexpr spi_host_device_t SHARP_SPI_HOST = SPI2_HOST;

// Sharp Memory LCD command bits when SPI is configured LSB-first.
static constexpr uint8_t SHARP_CMD_WRITE = 0x01;
static constexpr uint8_t SHARP_CMD_VCOM  = 0x02;
static constexpr uint8_t SHARP_CMD_CLEAR = 0x04;

SharpDisplay::~SharpDisplay() {
    deinit();
}

void SharpDisplay::lock() {
    if (mutex_) {
        xSemaphoreTake(reinterpret_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    }
}

void SharpDisplay::unlock() {
    if (mutex_) {
        xSemaphoreGive(reinterpret_cast<SemaphoreHandle_t>(mutex_));
    }
}

uint8_t SharpDisplay::command_byte(uint8_t cmd) const {
    return static_cast<uint8_t>(cmd | (vcom_ ? SHARP_CMD_VCOM : 0));
}

esp_err_t SharpDisplay::init() {
    Config cfg;
    return init(cfg);
}

esp_err_t SharpDisplay::init(const Config& cfg) {
    if (initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    cfg_ = cfg;
    if (cfg_.clock_hz <= 0 || cfg_.vcom_period_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) {
        return ESP_ERR_NO_MEM;
    }

    if (cfg_.disp_io >= 0) {
        gpio_config_t gpio_cfg = {};
        gpio_cfg.pin_bit_mask = 1ULL << cfg_.disp_io;
        gpio_cfg.mode = GPIO_MODE_OUTPUT;
        esp_err_t err = gpio_config(&gpio_cfg);
        if (err != ESP_OK) {
            deinit();
            return err;
        }
        gpio_set_level(static_cast<gpio_num_t>(cfg_.disp_io), 1);
    }

    if (cfg_.extmode_io >= 0) {
        gpio_config_t gpio_cfg = {};
        gpio_cfg.pin_bit_mask = 1ULL << cfg_.extmode_io;
        gpio_cfg.mode = GPIO_MODE_OUTPUT;
        esp_err_t err = gpio_config(&gpio_cfg);
        if (err != ESP_OK) {
            deinit();
            return err;
        }
        gpio_set_level(static_cast<gpio_num_t>(cfg_.extmode_io), 0);
    }

    // Sharp Memory LCD uses ACTIVE-HIGH SCS. Hardware SPI CS is active-low,
    // therefore SCS is controlled manually and remains LOW while idle.
    gpio_config_t cs_cfg = {};
    cs_cfg.pin_bit_mask = 1ULL << cfg_.cs_io;
    cs_cfg.mode = GPIO_MODE_OUTPUT;
    cs_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cs_cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    cs_cfg.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&cs_cfg);
    if (err != ESP_OK) {
        deinit();
        return err;
    }
    gpio_set_level(static_cast<gpio_num_t>(cfg_.cs_io), 0);
    esp_rom_delay_us(2);

    // The stable Arduino driver kept this compact 12 KiB framebuffer in fast
    // RAM. Prefer internal memory so dithering and dirty-row scans do not
    // contend with PSRAM ROM traffic on the other core.
    fb_ = static_cast<uint8_t*>(heap_caps_malloc(
        FRAMEBUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!fb_) {
        ESP_LOGW(TAG, "Internal framebuffer allocation failed; trying PSRAM");
        fb_ = static_cast<uint8_t*>(heap_caps_malloc(
            FRAMEBUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!fb_) {
        ESP_LOGE(TAG, "Framebuffer allocation failed (%u bytes)",
                 static_cast<unsigned>(FRAMEBUFFER_SIZE));
        deinit();
        return ESP_ERR_NO_MEM;
    }

    tx_capacity_ = 1 + HEIGHT * (1 + BYTES_PER_ROW + 1) + 1;
    tx_ = static_cast<uint8_t*>(heap_caps_malloc(
        tx_capacity_, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (!tx_) {
        ESP_LOGE(TAG, "DMA TX buffer allocation failed (%u bytes)",
                 static_cast<unsigned>(tx_capacity_));
        deinit();
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = cfg_.mosi_io;
    bus_cfg.miso_io_num = -1;
    bus_cfg.sclk_io_num = cfg_.sck_io;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = static_cast<int>(tx_capacity_);

    err = spi_bus_initialize(SHARP_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        owns_spi_bus_ = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        deinit();
        return err;
    }

    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = cfg_.clock_hz;
    dev_cfg.mode = 0;
    dev_cfg.spics_io_num = -1;
    dev_cfg.queue_size = 1;
    dev_cfg.flags = SPI_DEVICE_TXBIT_LSBFIRST;

    spi_device_handle_t dev = nullptr;
    err = spi_bus_add_device(SHARP_SPI_HOST, &dev_cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        deinit();
        return err;
    }
    spi_ = dev;
    initialized_ = true;

    clear(WHITE);
    err = clear_lcd_hw();
    if (err == ESP_OK) {
        err = refresh_full();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initial LCD refresh failed: %s", esp_err_to_name(err));
        deinit();
        return err;
    }

    if (cfg_.enable_vcom_task) {
        err = start_vcom_task();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not start VCOM task: %s", esp_err_to_name(err));
            deinit();
            return err;
        }
    }

    ESP_LOGI(TAG,
             "Sharp LS027B7DH01 ready: fb=%uB, tx=%uB DMA, SPI=%dHz, VCOM=%ums",
             static_cast<unsigned>(FRAMEBUFFER_SIZE),
             static_cast<unsigned>(tx_capacity_), cfg_.clock_hz,
             static_cast<unsigned>(cfg_.vcom_period_ms));
    return ESP_OK;
}

void SharpDisplay::deinit() {
    stop_vcom_task();

    if (spi_) {
        spi_bus_remove_device(reinterpret_cast<spi_device_handle_t>(spi_));
        spi_ = nullptr;
    }
    if (owns_spi_bus_) {
        spi_bus_free(SHARP_SPI_HOST);
        owns_spi_bus_ = false;
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

    tx_capacity_ = 0;
    initialized_ = false;
    vcom_ = false;
    std::memset(dirty_, 0, sizeof(dirty_));
}

void SharpDisplay::set_pixel_unlocked(int x, int y, Color color) {
    if (!fb_ || x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
        return;
    }

    uint8_t& value = fb_[y * BYTES_PER_ROW + (x >> 3)];
    const uint8_t mask = static_cast<uint8_t>(1U << (x & 7));
    if (color == WHITE) {
        value |= mask;
    } else {
        value &= static_cast<uint8_t>(~mask);
    }
    dirty_[y] = true;
}

void SharpDisplay::mark_dirty_row_unlocked(int y) {
    if (y >= 0 && y < HEIGHT) {
        dirty_[y] = true;
    }
}

void SharpDisplay::mark_dirty_range_unlocked(int y, int h) {
    if (h <= 0) {
        return;
    }
    const int y0 = std::max(0, y);
    const int y1 = std::min(HEIGHT, y + h);
    for (int row = y0; row < y1; ++row) {
        dirty_[row] = true;
    }
}

void SharpDisplay::draw_hline_unlocked(int x, int y, int w, Color color) {
    if (!fb_ || y < 0 || y >= HEIGHT || w <= 0) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (x + w > WIDTH) {
        w = WIDTH - x;
    }
    if (w <= 0) {
        return;
    }

    const int end = x + w;
    while (x < end && (x & 7)) {
        set_pixel_unlocked(x++, y, color);
    }

    const uint8_t fill = color == WHITE ? 0xFF : 0x00;
    while (x + 8 <= end) {
        fb_[y * BYTES_PER_ROW + (x >> 3)] = fill;
        x += 8;
    }
    while (x < end) {
        set_pixel_unlocked(x++, y, color);
    }
    dirty_[y] = true;
}

void SharpDisplay::fill_rect_unlocked(int x, int y, int w, int h, Color color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    const int y0 = std::max(0, y);
    const int y1 = std::min(HEIGHT, y + h);
    for (int row = y0; row < y1; ++row) {
        draw_hline_unlocked(x, row, w, color);
    }
}

void SharpDisplay::clear(Color color) {
    if (!fb_) {
        return;
    }
    lock();
    std::memset(fb_, color == WHITE ? 0xFF : 0x00, FRAMEBUFFER_SIZE);
    std::fill(std::begin(dirty_), std::end(dirty_), true);
    unlock();
}

void SharpDisplay::draw_pixel(int x, int y, Color color) {
    lock();
    set_pixel_unlocked(x, y, color);
    unlock();
}

void SharpDisplay::draw_hline(int x, int y, int w, Color color) {
    lock();
    draw_hline_unlocked(x, y, w, color);
    unlock();
}

void SharpDisplay::draw_vline(int x, int y, int h, Color color) {
    if (h <= 0) {
        return;
    }
    lock();
    const int y0 = std::max(0, y);
    const int y1 = std::min(HEIGHT, y + h);
    for (int row = y0; row < y1; ++row) {
        set_pixel_unlocked(x, row, color);
    }
    unlock();
}

void SharpDisplay::draw_line(int x0, int y0, int x1, int y1, Color color) {
    lock();
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    while (true) {
        set_pixel_unlocked(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = error * 2;
        if (e2 >= dy) {
            error += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            error += dx;
            y0 += sy;
        }
    }
    unlock();
}

void SharpDisplay::draw_rect(int x, int y, int w, int h, Color color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    lock();
    draw_hline_unlocked(x, y, w, color);
    if (h > 1) {
        draw_hline_unlocked(x, y + h - 1, w, color);
    }
    for (int row = y + 1; row < y + h - 1; ++row) {
        set_pixel_unlocked(x, row, color);
        if (w > 1) {
            set_pixel_unlocked(x + w - 1, row, color);
        }
    }
    unlock();
}

void SharpDisplay::fill_rect(int x, int y, int w, int h, Color color) {
    lock();
    fill_rect_unlocked(x, y, w, h, color);
    unlock();
}

void SharpDisplay::draw_circle(int cx, int cy, int radius, Color color) {
    if (radius < 0) {
        return;
    }
    lock();
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
        if (error < 0) {
            error += 2 * y + 1;
        } else {
            --x;
            error += 2 * (y - x) + 1;
        }
    }
    unlock();
}

void SharpDisplay::fill_circle(int cx, int cy, int radius, Color color) {
    if (radius < 0) {
        return;
    }
    lock();
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        draw_hline_unlocked(cx - x, cy + y, 2 * x + 1, color);
        draw_hline_unlocked(cx - x, cy - y, 2 * x + 1, color);
        draw_hline_unlocked(cx - y, cy + x, 2 * y + 1, color);
        draw_hline_unlocked(cx - y, cy - x, 2 * y + 1, color);
        ++y;
        if (error < 0) {
            error += 2 * y + 1;
        } else {
            --x;
            error += 2 * (y - x) + 1;
        }
    }
    unlock();
}

void SharpDisplay::draw_bitmap_1bpp(int x, int y, int w, int h,
                                    const uint8_t* bitmap, Color foreground,
                                    bool transparent_background,
                                    Color background) {
    if (!bitmap || w <= 0 || h <= 0) {
        return;
    }

    const int source_stride = (w + 7) / 8;
    lock();
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            // Conventional bitmap layout: MSB is the left-most pixel.
            const uint8_t source = bitmap[row * source_stride + (col >> 3)];
            const bool on = (source & (0x80U >> (col & 7))) != 0;
            if (on) {
                set_pixel_unlocked(x + col, y + row, foreground);
            } else if (!transparent_background) {
                set_pixel_unlocked(x + col, y + row, background);
            }
        }
    }
    unlock();
}

void SharpDisplay::draw_char_unlocked(int x, int y, char c, Color color, int scale) {
    scale = std::max(1, scale);
    for (int column = 0; column < 5; ++column) {
        const uint8_t bits = font5x7().glyph_column(c, column);
        for (int row = 0; row < 7; ++row) {
            if ((bits & (1U << row)) == 0) {
                continue;
            }
            if (scale == 1) {
                set_pixel_unlocked(x + column, y + row, color);
            } else {
                fill_rect_unlocked(x + column * scale, y + row * scale,
                                   scale, scale, color);
            }
        }
    }
}

void SharpDisplay::draw_char_5x7(int x, int y, char c, Color color, int scale) {
    lock();
    draw_char_unlocked(x, y, c, color, scale);
    unlock();
}

void SharpDisplay::draw_text(int x, int y, const char* text, Color color, int scale) {
    if (!text) {
        return;
    }
    scale = std::max(1, scale);
    lock();
    int cursor_x = x;
    while (*text) {
        if (*text == '\n') {
            y += 8 * scale;
            cursor_x = x;
            ++text;
            continue;
        }
        draw_char_unlocked(cursor_x, y, *text++, color, scale);
        cursor_x += 6 * scale;
    }
    unlock();
}

esp_err_t SharpDisplay::transmit_bytes(const uint8_t* data, size_t len) {
    if (!spi_ || !data || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    spi_transaction_t transaction = {};
    transaction.length = len * 8;
    transaction.tx_buffer = data;

    gpio_set_level(static_cast<gpio_num_t>(cfg_.cs_io), 1);
    esp_rom_delay_us(4);
    const esp_err_t err = spi_device_transmit(
        reinterpret_cast<spi_device_handle_t>(spi_), &transaction);
    esp_rom_delay_us(2);
    gpio_set_level(static_cast<gpio_num_t>(cfg_.cs_io), 0);
    esp_rom_delay_us(2);
    return err;
}

void SharpDisplay::record_refresh_unlocked(uint16_t rows, uint32_t bytes,
                                           uint32_t elapsed_us, bool full) {
    stats_.last_refresh_us = elapsed_us;
    stats_.last_rows = rows;
    stats_.last_bytes = bytes;
    ++stats_.total_refreshes;
    if (full) {
        ++stats_.full_refreshes;
    } else {
        ++stats_.partial_refreshes;
    }
    stats_.total_rows += rows;
    stats_.total_bytes += bytes;
}

esp_err_t SharpDisplay::clear_lcd_hw() {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    uint8_t command[2] = {command_byte(SHARP_CMD_CLEAR), 0x00};
    const esp_err_t err = transmit_bytes(command, sizeof(command));
    unlock();
    return err;
}

esp_err_t SharpDisplay::toggle_vcom_only() {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    vcom_ = !vcom_;
    uint8_t command[2] = {command_byte(0x00), 0x00};
    const esp_err_t err = transmit_bytes(command, sizeof(command));
    if (err == ESP_OK) {
        ++stats_.vcom_toggles;
    }
    unlock();
    return err;
}

esp_err_t SharpDisplay::refresh_rows(int y, int h) {
    if (!initialized_ || !fb_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (h <= 0) {
        return ESP_OK;
    }

    const int y0 = std::max(0, y);
    const int y1 = std::min(HEIGHT, y + h);
    if (y0 >= y1) {
        return ESP_OK;
    }

    const uint16_t rows = static_cast<uint16_t>(y1 - y0);
    const size_t needed = 1 + rows * (1 + BYTES_PER_ROW + 1) + 1;
    if (needed > tx_capacity_) {
        return ESP_ERR_NO_MEM;
    }

    lock();
    size_t position = 0;
    tx_[position++] = command_byte(SHARP_CMD_WRITE);
    for (int row = y0; row < y1; ++row) {
        tx_[position++] = static_cast<uint8_t>(row + 1);
        std::memcpy(&tx_[position], &fb_[row * BYTES_PER_ROW], BYTES_PER_ROW);
        position += BYTES_PER_ROW;
        tx_[position++] = 0x00;
    }
    tx_[position++] = 0x00;

    const int64_t start = esp_timer_get_time();
    const esp_err_t err = transmit_bytes(tx_, position);
    const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - start);
    if (err == ESP_OK) {
        for (int row = y0; row < y1; ++row) {
            dirty_[row] = false;
        }
        record_refresh_unlocked(rows, static_cast<uint32_t>(position), elapsed,
                                rows == HEIGHT);
    }
    unlock();
    return err;
}

esp_err_t SharpDisplay::refresh_full() {
    return refresh_rows(0, HEIGHT);
}

esp_err_t SharpDisplay::refresh_dirty() {
    if (!initialized_ || !fb_) {
        return ESP_ERR_INVALID_STATE;
    }

    lock();
    uint16_t rows = 0;
    for (bool row_dirty : dirty_) {
        if (row_dirty) {
            ++rows;
        }
    }
    if (rows == 0) {
        unlock();
        return ESP_OK;
    }

    const size_t needed = 1 + rows * (1 + BYTES_PER_ROW + 1) + 1;
    if (needed > tx_capacity_) {
        unlock();
        return ESP_ERR_NO_MEM;
    }

    size_t position = 0;
    tx_[position++] = command_byte(SHARP_CMD_WRITE);
    for (int row = 0; row < HEIGHT; ++row) {
        if (!dirty_[row]) {
            continue;
        }
        tx_[position++] = static_cast<uint8_t>(row + 1);
        std::memcpy(&tx_[position], &fb_[row * BYTES_PER_ROW], BYTES_PER_ROW);
        position += BYTES_PER_ROW;
        tx_[position++] = 0x00;
    }
    tx_[position++] = 0x00;

    const int64_t start = esp_timer_get_time();
    const esp_err_t err = transmit_bytes(tx_, position);
    const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - start);
    if (err == ESP_OK) {
        std::fill(std::begin(dirty_), std::end(dirty_), false);
        record_refresh_unlocked(rows, static_cast<uint32_t>(position), elapsed,
                                rows == HEIGHT);
    }
    unlock();
    return err;
}

SharpDisplay::Stats SharpDisplay::get_stats() {
    lock();
    const Stats copy = stats_;
    unlock();
    return copy;
}

void SharpDisplay::reset_stats() {
    lock();
    stats_ = {};
    unlock();
}

uint16_t SharpDisplay::dirty_row_count() {
    lock();
    uint16_t rows = 0;
    for (bool dirty : dirty_) {
        if (dirty) {
            ++rows;
        }
    }
    unlock();
    return rows;
}

esp_err_t SharpDisplay::start_vcom_task() {
    if (vcom_task_) {
        return ESP_ERR_INVALID_STATE;
    }

    vcom_task_stop_ = false;
    TaskHandle_t handle = nullptr;
    BaseType_t result;
    if (cfg_.vcom_task_core >= 0) {
        result = xTaskCreatePinnedToCore(
            &SharpDisplay::vcom_task_entry, "sharp_vcom",
            cfg_.vcom_task_stack, this, cfg_.vcom_task_priority,
            &handle, cfg_.vcom_task_core);
    } else {
        result = xTaskCreate(
            &SharpDisplay::vcom_task_entry, "sharp_vcom",
            cfg_.vcom_task_stack, this, cfg_.vcom_task_priority,
            &handle);
    }

    if (result != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    vcom_task_ = handle;
    return ESP_OK;
}

void SharpDisplay::stop_vcom_task() {
    if (!vcom_task_) {
        return;
    }
    vcom_task_stop_ = true;
    vTaskDelete(reinterpret_cast<TaskHandle_t>(vcom_task_));
    vcom_task_ = nullptr;
}

void SharpDisplay::vcom_task_entry(void* arg) {
    auto* display = static_cast<SharpDisplay*>(arg);
    const TickType_t period = pdMS_TO_TICKS(display->cfg_.vcom_period_ms);
    while (!display->vcom_task_stop_) {
        vTaskDelay(period);
        if (display->vcom_task_stop_) {
            break;
        }
        const esp_err_t err = display->toggle_vcom_only();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "VCOM toggle failed: %s", esp_err_to_name(err));
        }
    }
    display->vcom_task_ = nullptr;
    vTaskDelete(nullptr);
}

} // namespace pogopo::gfx
