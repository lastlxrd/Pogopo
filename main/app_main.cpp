#include "i2c_bus.h"
#include "peripheral_tests.h"
#include "tasks.h"
#include "board_pins.h"
#include "pogopo_sharp.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "app";

static pogopo::SharpDisplay g_display;

static void sharp_vcom_task(void*) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (g_display.ok()) {
            esp_err_t err = g_display.toggle_vcom_only();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Sharp VCOM toggle failed: %s", esp_err_to_name(err));
            }
        }
    }
}

static void start_display_test() {
    pogopo::SharpDisplay::Config cfg;
    cfg.sck_io = board::LCD_SCK;
    cfg.mosi_io = board::LCD_MOSI;
    cfg.cs_io = board::LCD_CS;
    cfg.disp_io = -1;     // DISP is pulled high on the board for now
    cfg.extmode_io = -1;  // EXTMODE is tied low on the board
    cfg.clock_hz = 2000000;  // datasheet-safe bring-up speed

    esp_err_t err = g_display.init(cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Sharp display init failed: %s", esp_err_to_name(err));
        return;
    }

    g_display.clear(pogopo::SharpDisplay::WHITE);
    g_display.draw_rect(0, 0, pogopo::SharpDisplay::WIDTH, pogopo::SharpDisplay::HEIGHT, pogopo::SharpDisplay::BLACK);
    g_display.draw_text(16, 18, "pogopoOS2.0", pogopo::SharpDisplay::BLACK, 2);
    g_display.draw_text(16, 48, "ESP-IDF Sharp driver", pogopo::SharpDisplay::BLACK, 1);
    g_display.draw_text(16, 66, "LS027B7DH01 400x240", pogopo::SharpDisplay::BLACK, 1);
    g_display.draw_text(16, 84, "Framebuffer: PSRAM", pogopo::SharpDisplay::BLACK, 1);

    for (int i = 0; i < 8; ++i) {
        g_display.fill_rect(16 + i * 24, 110, 16, 16, (i & 1) ? pogopo::SharpDisplay::WHITE : pogopo::SharpDisplay::BLACK);
        g_display.draw_rect(16 + i * 24, 110, 16, 16, pogopo::SharpDisplay::BLACK);
    }

    ESP_ERROR_CHECK(g_display.refresh_full());

    BaseType_t task_ok = xTaskCreate(
        sharp_vcom_task, "sharp_vcom", 2048, nullptr, 3, nullptr);
    if (task_ok != pdPASS) {
        ESP_LOGW(TAG, "Could not start Sharp VCOM task");
    }
}

extern "C" void app_main(void) {
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(nullptr, &flash_size));

    ESP_LOGI(TAG, "pogopoOS2.0 peripheral test");
    ESP_LOGI(TAG, "ESP32-S3 cores=%d rev=%d flash=%u MB",
             chip.cores, chip.revision,
             static_cast<unsigned>(flash_size / (1024 * 1024)));

    ESP_ERROR_CHECK(i2c_bus_init());
    i2c_scan();
    run_peripheral_tests();
    start_display_test();
    start_system_tasks();

    ESP_LOGI(TAG, "Tests running. Press console buttons and watch logs.");
}
