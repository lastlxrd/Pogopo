#include "i2c_bus.h"
#include "peripheral_tests.h"
#include "tasks.h"
#include "board_pins.h"
#include "pogopo_graphics.h"

#include <cstdio>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "app";
static const char* GFX_TAG = "gfx_demo";

static pogopo::SharpDisplay g_display;
static pogopo::Canvas g_canvas(g_display);

static void draw_static_graphics_demo() {
    using Canvas = pogopo::Canvas;

    g_canvas.clear(Canvas::WHITE);
    g_canvas.rect(0, 0, g_canvas.width(), g_canvas.height(), Canvas::BLACK);
    g_canvas.text(16, 14, "pogopoOS2.0 STEP2", Canvas::BLACK, 2);
    g_canvas.text(16, 42, "ESP-IDF graphics engine", Canvas::BLACK, 1);
    g_canvas.text(16, 58, "PSRAM framebuffer + dirty rows", Canvas::BLACK, 1);

    // Primitive demo.
    g_canvas.line(18, 82, 82, 112, Canvas::BLACK);
    g_canvas.rect(96, 80, 38, 28, Canvas::BLACK);
    g_canvas.fill_rect(148, 80, 38, 28, Canvas::BLACK);
    g_canvas.circle(218, 94, 15, Canvas::BLACK);
    g_canvas.fill_circle(270, 94, 15, Canvas::BLACK);

    // Animation lane. Only its changed rows should be sent each frame.
    g_canvas.rect(16, 124, 368, 34, Canvas::BLACK);
    g_canvas.text(22, 132, "DIRTY ROW ANIMATION", Canvas::BLACK, 1);

    g_canvas.text(16, 176, "FPS:", Canvas::BLACK, 1);
    g_canvas.text(16, 190, "Frame us:", Canvas::BLACK, 1);
    g_canvas.text(16, 204, "Rows:", Canvas::BLACK, 1);
    g_canvas.text(16, 218, "Bytes:", Canvas::BLACK, 1);

    ESP_ERROR_CHECK(g_canvas.present_full());
}

static void graphics_demo_task(void*) {
    using Canvas = pogopo::Canvas;

    constexpr int square_w = 18;
    constexpr int square_h = 18;
    constexpr int lane_x0 = 190;
    constexpr int lane_x1 = 358;
    constexpr int lane_y = 132;

    int x = lane_x0;
    int previous_x = x;
    int direction = 1;

    uint32_t frames = 0;
    uint32_t last_fps = 0;
    int64_t stats_window_start = esp_timer_get_time();
    TickType_t wake = xTaskGetTickCount();

    while (true) {
        // Erase only the previous square and draw the new one. The driver marks
        // just these rows dirty and packs all dirty lines into one SPI command.
        g_canvas.fill_rect(previous_x, lane_y, square_w, square_h, Canvas::WHITE);
        g_canvas.rect(previous_x, lane_y, square_w, square_h, Canvas::WHITE);

        x += direction * 3;
        if (x >= lane_x1 - square_w) {
            x = lane_x1 - square_w;
            direction = -1;
        } else if (x <= lane_x0) {
            x = lane_x0;
            direction = 1;
        }

        g_canvas.fill_rect(x, lane_y, square_w, square_h, Canvas::BLACK);
        previous_x = x;

        esp_err_t err = g_canvas.present();
        if (err != ESP_OK) {
            ESP_LOGW(GFX_TAG, "Partial refresh failed: %s", esp_err_to_name(err));
        }
        ++frames;

        const int64_t now = esp_timer_get_time();
        if (now - stats_window_start >= 1000000) {
            last_fps = frames;
            frames = 0;
            stats_window_start = now;

            const auto stats = g_display.get_stats();
            char value[32];

            // Clear only the value column; labels remain untouched.
            g_canvas.fill_rect(82, 174, 290, 54, Canvas::WHITE);

            std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(last_fps));
            g_canvas.text(82, 176, value, Canvas::BLACK, 1);

            std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(stats.last_refresh_us));
            g_canvas.text(82, 190, value, Canvas::BLACK, 1);

            std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(stats.last_rows));
            g_canvas.text(82, 204, value, Canvas::BLACK, 1);

            std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(stats.last_bytes));
            g_canvas.text(82, 218, value, Canvas::BLACK, 1);

            ESP_ERROR_CHECK_WITHOUT_ABORT(g_canvas.present());

            ESP_LOGI(GFX_TAG,
                     "FPS=%u last=%uus rows=%u bytes=%u total_refresh=%llu total_rows=%llu total_bytes=%llu vcom=%llu",
                     static_cast<unsigned>(last_fps),
                     static_cast<unsigned>(stats.last_refresh_us),
                     static_cast<unsigned>(stats.last_rows),
                     static_cast<unsigned>(stats.last_bytes),
                     static_cast<unsigned long long>(stats.total_refreshes),
                     static_cast<unsigned long long>(stats.total_rows),
                     static_cast<unsigned long long>(stats.total_bytes),
                     static_cast<unsigned long long>(stats.vcom_toggles));
        }

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(33)); // target about 30 FPS
    }
}

static void start_graphics_step2() {
    pogopo::SharpDisplay::Config cfg;
    cfg.sck_io = board::LCD_SCK;
    cfg.mosi_io = board::LCD_MOSI;
    cfg.cs_io = board::LCD_CS;
    cfg.disp_io = -1;
    cfg.extmode_io = -1;
    cfg.clock_hz = 2000000;
    cfg.enable_vcom_task = true;
    cfg.vcom_period_ms = 500;
    cfg.vcom_task_priority = 4;
    cfg.vcom_task_core = 0;

    const esp_err_t err = g_display.init(cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Graphics init failed: %s", esp_err_to_name(err));
        return;
    }

    draw_static_graphics_demo();
    g_display.reset_stats();

    const BaseType_t result = xTaskCreatePinnedToCore(
        graphics_demo_task, "graphics_demo", 4096, nullptr, 3, nullptr, 1);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Could not start graphics demo task");
    }
}

extern "C" void app_main(void) {
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(nullptr, &flash_size));

    ESP_LOGI(TAG, "pogopoOS2.0 GRAPHICS STEP2");
    ESP_LOGI(TAG, "ESP32-S3 cores=%d rev=%d flash=%u MB",
             chip.cores, chip.revision,
             static_cast<unsigned>(flash_size / (1024 * 1024)));

    ESP_ERROR_CHECK(i2c_bus_init());
    i2c_scan();
    run_peripheral_tests();
    start_graphics_step2();
    start_system_tasks();

    ESP_LOGI(TAG, "STEP2 running: VCOM task + Canvas + dirty-row animation + benchmark");
}
