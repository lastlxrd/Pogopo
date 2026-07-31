#include "i2c_bus.h"
#include "peripheral_tests.h"
#include "tasks.h"
#include "board_pins.h"
#include "system_state.h"
#include "apps/demo_apps.h"

#include "pogopo_app.h"
#include "pogopo_gui.h"
#include "pogopo/gfx/gfx.h"
#include "pogopo_input.h"
#include "pogopo_haptics.h"
#include "pogopo_audio.h"

#include <algorithm>
#include <cstdint>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "app";

static pogopo::Graphics g_gfx;
static pogopo::Input g_input;
static pogopo::Haptics g_haptics;
static pogopo::Audio g_audio;
static pogopo::AppManager g_app_manager(g_gfx, g_input, g_haptics, g_audio);

static pogopo::demo::LauncherApp g_launcher_app;
static pogopo::demo::GraphicsDemoApp g_graphics_app;
static pogopo::demo::InputMonitorApp g_input_app;
static pogopo::demo::HapticsLabApp g_haptics_app;
static pogopo::demo::AudioLabApp g_audio_app;
static pogopo::demo::AboutApp g_about_app;

namespace {

esp_err_t start_graphics() {
    pogopo::Graphics::Config config;
    config.sck_io = board::LCD_SCK;
    config.mosi_io = board::LCD_MOSI;
    config.cs_io = board::LCD_CS;
    config.disp_io = -1;
    config.extmode_io = -1;
    config.clock_hz = 2000000;
    config.enable_vcom_task = true;
    config.vcom_period_ms = 500;
    config.vcom_task_priority = 4;
    config.vcom_task_core = 0;
    return g_gfx.begin(config);
}

esp_err_t start_haptics() {
    pogopo::Haptics::Config config;
    config.motor_io = board::VIBRO;
    config.active_high = true;
    config.task_priority = 4;
    config.task_core = 0;
    return g_haptics.begin(config);
}


esp_err_t start_audio() {
    pogopo::Audio::Config config;
    config.dout_io = board::AUDIO_DOUT;
    config.bclk_io = board::AUDIO_BCLK;
    config.lrck_io = board::AUDIO_LRCK;
    config.sample_rate = 32768;
    config.master_volume = 68;
    config.dma_desc_num = 6;
    config.dma_frame_num = 256;
    config.render_frames = 256;
    config.task_priority = 6;
    config.task_core = 0;
    return g_audio.begin(config);
}

esp_err_t start_input() {
    pogopo::Input::Config config;
    config.bus = i2c_bus_handle();
    config.address = 0x20;
    config.interrupt_io = board::TCA9555_INT;
    config.active_low = true;
    config.poll_period_ms = 4;
    config.debounce_samples = 3;
    config.repeat_delay_ms = 450;
    config.repeat_period_ms = 100;
    config.long_press_ms = 700;
    config.task_priority = 5;
    config.task_core = 0;
    return g_input.begin(config);
}

void os_task(void*) {
    int64_t last_us = esp_timer_get_time();
    TickType_t wake = xTaskGetTickCount();

    g_app_manager.registerApp(g_launcher_app, true);
    g_app_manager.registerApp(g_graphics_app);
    g_app_manager.registerApp(g_input_app);
    g_app_manager.registerApp(g_haptics_app);
    g_app_manager.registerApp(g_audio_app);
    g_app_manager.registerApp(g_about_app);
    g_app_manager.start("launcher");
    g_haptics.play(pogopo::HapticEffect::Confirm);
    g_audio.play(pogopo::AudioEffect::Startup);

    while (true) {
        const int64_t now_us = esp_timer_get_time();
        const uint32_t dt_ms = static_cast<uint32_t>(std::clamp<int64_t>((now_us - last_us) / 1000, 0, 100));
        last_us = now_us;

        g_app_manager.processInput();
        g_app_manager.update(dt_ms);
        const esp_err_t render_error = g_app_manager.render();
        if (render_error != ESP_OK) {
            ESP_LOGW(TAG, "GUI render failed: %s", esp_err_to_name(render_error));
        }

        g_system_state.buttons_port.store(g_input.rawPort());
        g_system_state.buttons_ok.store(g_input.ok());
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(8));
    }
}

} // namespace

extern "C" void app_main(void) {
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(nullptr, &flash_size));

    ESP_LOGI(TAG, "pogopoOS2.0 AUDIO ENGINE STEP6");
    ESP_LOGI(TAG, "ESP32-S3 cores=%d rev=%d flash=%u MB",
             chip.cores, chip.revision,
             static_cast<unsigned>(flash_size / (1024 * 1024)));

    ESP_ERROR_CHECK(i2c_bus_init());
    i2c_scan();
    run_peripheral_tests();

    ESP_ERROR_CHECK(start_graphics());
    ESP_ERROR_CHECK(start_haptics());
    ESP_ERROR_CHECK(start_audio());
    ESP_ERROR_CHECK(start_input());

    if (xTaskCreatePinnedToCore(os_task, "pogopo_os", 8192, nullptr, 3, nullptr, 1) != pdPASS) {
        ESP_LOGE(TAG, "Could not create pogopoOS task");
    }

    start_system_tasks();
    ESP_LOGI(TAG, "STEP6 ready: native I2S audio mixer + GUI audio lab + louder haptics");
}
