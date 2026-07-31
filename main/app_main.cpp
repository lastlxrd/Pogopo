#include "i2c_bus.h"
#include "peripheral_tests.h"
#include "tasks.h"
#include "board_pins.h"
#include "pogopo/gfx/gfx.h"

#include <cstdio>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "app";
static const char* GFX_TAG = "gfx_step3";

static pogopo::Graphics g_gfx;

// 16x16 transparent sprite, MSB-first, two bytes per row.
static constexpr uint8_t kPogopoSpriteData[] = {
    0x03,0xC0, 0x0F,0xF0, 0x1C,0x38, 0x30,0x0C,
    0x63,0xC6, 0x47,0xE2, 0xC6,0x63, 0xC0,0x03,
    0xC0,0x03, 0xC4,0x23, 0x66,0x66, 0x33,0xCC,
    0x1C,0x38, 0x0F,0xF0, 0x03,0xC0, 0x00,0x00,
};
static const pogopo::Bitmap kPogopoSprite =
    pogopo::gfx::make_bitmap_1bpp(16, 16, kPogopoSpriteData);

static void draw_static_demo() {
    using G = pogopo::Graphics;

    g_gfx.clear(G::WHITE);
    g_gfx.drawRect(0, 0, g_gfx.width(), g_gfx.height(), G::BLACK);
    g_gfx.drawText(14, 12, "pogopoOS2.0 STEP3", G::BLACK, 2);
    g_gfx.drawText(14, 39, "pogopo::gfx  /  Graphics facade", G::BLACK, 1);
    g_gfx.drawText(14, 54, "Canvas Font Bitmap Sprite Clip", G::BLACK, 1);

    // GFX facade primitive test.
    g_gfx.drawLine(18, 78, 74, 104, G::BLACK);
    g_gfx.drawRect(88, 75, 34, 26, G::BLACK);
    g_gfx.fillRect(134, 75, 34, 26, G::BLACK);
    g_gfx.drawCircle(202, 88, 14, G::BLACK);
    g_gfx.fillCircle(246, 88, 14, G::BLACK);
    g_gfx.drawBitmap(282, 72, kPogopoSprite, G::BLACK, true);

    // Clip test area. The moving sprite is deliberately allowed to cross the
    // logical edges, but pixels outside this rectangle must never appear.
    g_gfx.drawRect(14, 116, 372, 42, G::BLACK);
    g_gfx.drawText(20, 122, "CLIPPED SPRITE + DIRTY ROWS", G::BLACK, 1);

    g_gfx.drawText(14, 174, "FPS:", G::BLACK, 1);
    g_gfx.drawText(14, 188, "Refresh us:", G::BLACK, 1);
    g_gfx.drawText(14, 202, "Rows:", G::BLACK, 1);
    g_gfx.drawText(14, 216, "Bytes:", G::BLACK, 1);

    ESP_ERROR_CHECK(g_gfx.presentFull());
}

static void graphics_step3_task(void*) {
    using G = pogopo::Graphics;

    constexpr pogopo::Rect clip_area{168, 136, 202, 17};
    constexpr int sprite_y = 136;
    constexpr int x_min = 158; // deliberately 10 px outside clip
    constexpr int x_max = 365; // deliberately outside clip on right

    pogopo::Sprite sprite;
    sprite.bitmap = kPogopoSprite;
    sprite.x = x_min;
    sprite.y = sprite_y;
    sprite.foreground = G::BLACK;
    sprite.transparent_background = true;

    int previous_x = sprite.x;
    int direction = 1;
    uint32_t frames = 0;
    uint32_t shown_fps = 0;
    int64_t stats_window_start = esp_timer_get_time();
    TickType_t wake = xTaskGetTickCount();

    while (true) {
        g_gfx.set_clip(clip_area);
        g_gfx.fillRect(previous_x, sprite_y, 16, 16, G::WHITE);

        sprite.x += direction * 3;
        if (sprite.x >= x_max) { sprite.x = x_max; direction = -1; }
        else if (sprite.x <= x_min) { sprite.x = x_min; direction = 1; }

        g_gfx.drawSprite(sprite);
        g_gfx.reset_clip();
        previous_x = sprite.x;

        const esp_err_t err = g_gfx.present();
        if (err != ESP_OK) {
            ESP_LOGW(GFX_TAG, "present failed: %s", esp_err_to_name(err));
        }
        ++frames;

        const int64_t now = esp_timer_get_time();
        if (now - stats_window_start >= 1000000) {
            shown_fps = frames;
            frames = 0;
            stats_window_start = now;

            const auto stats = g_gfx.stats();
            char value[32];
            g_gfx.fillRect(102, 171, 270, 56, G::WHITE);

            std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(shown_fps));
            g_gfx.drawText(102, 174, value, G::BLACK, 1);
            std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(stats.last_refresh_us));
            g_gfx.drawText(102, 188, value, G::BLACK, 1);
            std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(stats.last_rows));
            g_gfx.drawText(102, 202, value, G::BLACK, 1);
            std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(stats.last_bytes));
            g_gfx.drawText(102, 216, value, G::BLACK, 1);
            ESP_ERROR_CHECK_WITHOUT_ABORT(g_gfx.present());

            ESP_LOGI(GFX_TAG,
                     "FPS=%u refresh=%uus rows=%u bytes=%u total=%llu vcom=%llu clip=[%d,%d,%d,%d]",
                     static_cast<unsigned>(shown_fps),
                     static_cast<unsigned>(stats.last_refresh_us),
                     static_cast<unsigned>(stats.last_rows),
                     static_cast<unsigned>(stats.last_bytes),
                     static_cast<unsigned long long>(stats.total_refreshes),
                     static_cast<unsigned long long>(stats.vcom_toggles),
                     clip_area.x, clip_area.y, clip_area.w, clip_area.h);
        }

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(33));
    }
}

static void start_graphics_step3() {
    pogopo::Graphics::Config cfg;
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

    const esp_err_t err = g_gfx.begin(cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Graphics begin failed: %s", esp_err_to_name(err));
        return;
    }

    draw_static_demo();
    g_gfx.resetStats();

    const BaseType_t result = xTaskCreatePinnedToCore(
        graphics_step3_task, "gfx_step3_demo", 4096, nullptr, 3, nullptr, 1);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Could not start STEP3 graphics task");
    }
}

extern "C" void app_main(void) {
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(nullptr, &flash_size));

    ESP_LOGI(TAG, "pogopoOS2.0 GRAPHICS STEP3");
    ESP_LOGI(TAG, "ESP32-S3 cores=%d rev=%d flash=%u MB",
             chip.cores, chip.revision,
             static_cast<unsigned>(flash_size / (1024 * 1024)));

    ESP_ERROR_CHECK(i2c_bus_init());
    i2c_scan();
    run_peripheral_tests();
    start_graphics_step3();
    start_system_tasks();

    ESP_LOGI(TAG, "STEP3 running: namespace + Graphics facade + Canvas + Font + Bitmap + Sprite + Clip");
}
