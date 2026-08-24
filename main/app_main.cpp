#include "i2c_bus.h"
#include "peripheral_tests.h"
#include "tasks.h"
#include "board_pins.h"
#include "system_state.h"
#include "apps/demo_apps.h"
#include "apps/gameboy_apps.h"
#include "apps/playdate_apps.h"

#include "pogopo_app.h"
#include "pogopo_gui.h"
#include "pogopo/gfx/gfx.h"
#include "pogopo_input.h"
#include "pogopo_haptics.h"
#include "pogopo_audio.h"
#include "pogopo_storage.h"
#include "pogopo_imu.h"
#include "pogopo_power.h"
#include "pogopo_settings.h"
#include "pogopo_gameboy.h"
#include "pogopo_playdate.h"

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
static pogopo::Storage g_storage;
static pogopo::Imu g_imu;
static pogopo::Power g_power;
static pogopo::Settings g_settings;
static pogopo::GameBoy g_gameboy;
static pogopo::AppManager g_app_manager(g_gfx, g_input, g_haptics, g_audio, g_storage, g_imu, g_power, g_settings);

static pogopo::demo::LauncherApp g_launcher_app;
static pogopo::demo::GraphicsDemoApp g_graphics_app;
static pogopo::demo::InputMonitorApp g_input_app;
static pogopo::demo::HapticsLabApp g_haptics_app;
static pogopo::demo::AudioLabApp g_audio_app;
static pogopo::demo::WavPlayerApp g_wav_app;
static pogopo::demo::MotionLabApp g_motion_app;
static pogopo::demo::PowerStatusApp g_power_app;
static pogopo::demo::SettingsApp g_settings_app;
static pogopo::demo::AboutApp g_about_app;
static pogopo::demo::GameBoyApp g_gameboy_app(g_gameboy);
static pogopo::demo::GameBoyBrowserApp g_gameboy_browser_app(g_gameboy_app);
static pogopo::demo::PogoDateApp g_pdsnake_app(
    pogopo::playdate::Game::PDSnake, "pogodate", "PogoDate Snake",
    "PDSNAKE 1.2");
static pogopo::demo::PogoDateApp g_celeste_app(
    pogopo::playdate::Game::Celeste, "pogodate_celeste", "Celeste Classic",
    "CELESTE CLASSIC 1.0.3");
static pogopo::demo::PogoDateApp g_pogodate_player(
    pogopo::playdate::Game::External, "pogodate_player", "Playdate SD Game",
    "SD PACKAGE");
static pogopo::demo::PogoDateBrowserApp g_pogodate_browser(g_pogodate_player);

namespace {

esp_err_t start_graphics() {
    pogopo::Graphics::Config config;
    config.sck_io = board::LCD_SCK;
    config.mosi_io = board::LCD_MOSI;
    config.cs_io = board::LCD_CS;
    config.disp_io = -1;
    config.extmode_io = -1;
    // Same clock that was stable in the Arduino/Adafruit_SharpMem build.
    // At 2 MHz a full 400x240 transfer alone took about 49 ms.
    config.clock_hz = 14000000;
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
    const esp_err_t err = g_haptics.begin(config);
    if (err == ESP_OK) g_haptics.setEnabled(g_settings.hapticsEnabled());
    return err;
}


esp_err_t start_audio() {
    pogopo::Audio::Config config;
    config.dout_io = board::AUDIO_DOUT;
    config.bclk_io = board::AUDIO_BCLK;
    config.lrck_io = board::AUDIO_LRCK;
    config.sample_rate = 32768;
    config.master_volume = g_settings.volume();
    // Match the stable Arduino Game Boy I2S path.
    config.dma_desc_num = 12;
    config.dma_frame_num = 512;
    config.render_frames = 512;
    config.realtime_buffer_frames = 4096;
    config.task_priority = 8;
    config.task_stack = 8192;
    config.task_core = 0;
    const esp_err_t err = g_audio.begin(config);
    if (err == ESP_OK) g_audio.setEnabled(g_settings.audioEnabled());
    return err;
}


esp_err_t start_gameboy() {
    pogopo::GameBoy::Config config;
    config.internal_rom_arena_bytes = 128U * 1024U;
    config.internal_rom_arena_min_bytes = 32U * 1024U;
    config.internal_rom_headroom_bytes = 128U * 1024U;
    config.internal_rom_limit = 256U * 1024U;
    config.save_flush_interval_ms = 0; // Flush on exit/power-off, never mid-frame.
    config.requested_cache_pages = 8;
    // The Sharp frontend can present about 30 full scaled frames per second.
    // Skip only Peanut's expensive LCD drawing on alternate frames while the
    // CPU/APU still emulate every 59.7 Hz Game Boy frame.
    config.peanut_frame_skip = true;
    config.display_divider = 1;
    config.dither = true;
    config.task_priority = 6;
    config.task_core = 1;
    config.task_stack = 8192;
    config.audio_task_priority = 7;
    config.audio_task_core = 0;
    config.audio_task_stack = 4096;
    return g_gameboy.begin(g_audio, config);
}

esp_err_t start_storage() {
    pogopo::Storage::Config config;
    config.clk_io = board::SD_CLK; config.cmd_io = board::SD_CMD;
    config.d0_io = board::SD_D0; config.d1_io = board::SD_D1;
    config.d2_io = board::SD_D2; config.d3_io = board::SD_D3;
    config.mount_point = "/sdcard";
    config.max_files = 16;
    return g_storage.begin(config);
}

esp_err_t start_imu() {
    pogopo::Imu::Config config;
    config.bus = i2c_bus_handle(); config.address = 0x68;
    config.interrupt_io = board::IMU_INT; config.sample_period_ms = 20;
    // Keep Core 1 for input + emulator. BMI270 comfortably fits on Core 0
    // underneath the blocking priority-8 I2S writer.
    config.task_core = 0;
    return g_imu.begin(config);
}

esp_err_t start_power() {
    pogopo::Power::Config config;
    config.bus = i2c_bus_handle(); config.charger_address = 0x6B;
    config.power_button_io = board::POWER_BUTTON;
    config.charger_int_io = board::CHARGER_INT;
    config.battery_measure_io = board::BAT_MEAS;
    config.battery_gate_io = board::BAT_GATE;
    config.short_press_min_ms = 60;
    config.shutdown_hold_ms = 2000;
    config.task_core = 0;
    return g_power.begin(config);
}

void draw_power_message(const char* title, const char* line1, const char* line2) {
    auto& canvas = g_gfx.canvas();
    g_gfx.set_clip({0, 0, g_gfx.width(), g_gfx.height()});
    canvas.clear_clip(pogopo::gfx::WHITE);
    canvas.draw_rect(16, 18, 368, 204, pogopo::gfx::BLACK);
    canvas.draw_text(42, 43, title, pogopo::gfx::font5x7(), pogopo::gfx::BLACK, 2);
    canvas.draw_line(34, 74, 366, 74, pogopo::gfx::BLACK);
    canvas.draw_text(37, 101, line1, pogopo::gfx::font5x7(), pogopo::gfx::BLACK);
    canvas.draw_text(37, 124, line2, pogopo::gfx::font5x7(), pogopo::gfx::BLACK);
    canvas.draw_text(37, 178, "GPIO17 / BQ24295 BATFET", pogopo::gfx::font5x7(), pogopo::gfx::BLACK);
    g_gfx.reset_clip();
    g_gfx.present();
}

void handle_power_event(const pogopo::power::Event& event) {
    if (event.type == pogopo::power::EventType::ShortPress) {
        g_app_manager.toggleSystemMenu();
        return;
    }

    if (event.type == pogopo::power::EventType::UsbBlocked) {
        g_haptics.play(pogopo::HapticEffect::Alert);
        g_audio.play(pogopo::AudioEffect::Error);
        draw_power_message("USB CONNECTED", "UNPLUG TYPE-C TO POWER OFF", "RELEASE POWER BUTTON");
        g_power.waitForRelease(8000);
        g_app_manager.invalidate();
        return;
    }

    g_haptics.play(pogopo::HapticEffect::Heavy);
    g_gameboy.flushSave();
    g_audio.play(pogopo::AudioEffect::Confirm);
    draw_power_message("POWER OFF", "RELEASE POWER BUTTON", "ENTERING BQ SHIP MODE...");
    g_power.waitForRelease(8000); // QON must be released or the charger can wake again immediately.
    vTaskDelay(pdMS_TO_TICKS(120));
    g_audio.stopAll();

    // A Memory LCD keeps its pixels without power. Clear both the software
    // framebuffer and the physical panel before BATFET is disabled so the
    // previous POWER OFF card cannot reappear during the next boot.
    g_gfx.display().clear(pogopo::gfx::WHITE);
    const esp_err_t lcd_clear_error = g_gfx.display().clear_lcd_hw();
    if (lcd_clear_error != ESP_OK) {
        ESP_LOGW(TAG, "LCD pre-shutdown clear failed: %s",
                 esp_err_to_name(lcd_clear_error));
    }

    const esp_err_t err = g_power.enterShipMode();
    if (err != ESP_OK) {
        draw_power_message("POWER ERROR", esp_err_to_name(err), "PRESS RESET OR TRY AGAIN");
    }
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
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
    config.task_priority = 7;
    config.task_core = 1;
    return g_input.begin(config);
}

void os_task(void*) {
    int64_t last_us = esp_timer_get_time();
    uint32_t dt_remainder_us = 0;
    TickType_t wake = xTaskGetTickCount();
    const TickType_t loop_period = pdMS_TO_TICKS(8);
    uint32_t settings_dirty_ms = 0;

    g_app_manager.registerApp(g_launcher_app, true);
    g_app_manager.registerApp(g_gameboy_browser_app);
    g_app_manager.registerApp(g_gameboy_app);
    g_app_manager.registerApp(g_pogodate_browser);
    g_app_manager.registerApp(g_pogodate_player);
    g_app_manager.registerApp(g_pdsnake_app);
    g_app_manager.registerApp(g_celeste_app);
    g_app_manager.registerApp(g_graphics_app);
    g_app_manager.registerApp(g_input_app);
    g_app_manager.registerApp(g_haptics_app);
    g_app_manager.registerApp(g_audio_app);
    g_app_manager.registerApp(g_wav_app);
    g_app_manager.registerApp(g_motion_app);
    g_app_manager.registerApp(g_power_app);
    g_app_manager.registerApp(g_settings_app);
    g_app_manager.registerApp(g_about_app);
    g_app_manager.start("launcher");
    g_haptics.play(pogopo::HapticEffect::Confirm);
    if (g_settings.uiSoundsEnabled()) g_audio.play(pogopo::AudioEffect::Startup);

    while (true) {
        const int64_t now_us = esp_timer_get_time();
        const uint32_t elapsed_us = static_cast<uint32_t>(
            std::clamp<int64_t>(now_us - last_us, 0, 100000));
        last_us = now_us;
        const uint32_t elapsed_with_remainder = elapsed_us + dt_remainder_us;
        const uint32_t dt_ms = elapsed_with_remainder / 1000U;
        dt_remainder_us = elapsed_with_remainder % 1000U;

        pogopo::power::Event power_event;
        if (g_power.nextEvent(power_event, 0)) handle_power_event(power_event);

        g_app_manager.processInput();
        g_app_manager.update(dt_ms);

        if (g_settings.dirty()) {
            settings_dirty_ms += dt_ms;
            if (settings_dirty_ms >= 1200U) {
                const esp_err_t settings_err = g_settings.save();
                if (settings_err != ESP_OK) {
                    ESP_LOGW(TAG, "NVS auto-save failed: %s", esp_err_to_name(settings_err));
                }
                settings_dirty_ms = 0;
            }
        } else {
            settings_dirty_ms = 0;
        }
        const esp_err_t render_error = g_app_manager.render();
        if (render_error != ESP_OK) {
            ESP_LOGW(TAG, "GUI render failed: %s", esp_err_to_name(render_error));
        }

        g_system_state.buttons_port.store(g_input.rawPort());
        g_system_state.buttons_ok.store(g_input.ok());
        const TickType_t now_tick = xTaskGetTickCount();
        if (now_tick - wake >= loop_period) {
            // vTaskDelayUntil() returns immediately forever after a long Lua
            // frame leaves its wake time behind. Let IDLE0 run so a slow PDX
            // cannot starve the task watchdog, then restart the schedule.
            vTaskDelay(1);
            wake = xTaskGetTickCount();
        } else {
            vTaskDelayUntil(&wake, loop_period);
        }
    }
}

} // namespace

extern "C" void app_main(void) {
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(nullptr, &flash_size));

    ESP_LOGI(TAG, "pogopoOS2.0 STEP11.6.20 POGODATE OBJECT SCENES");
    ESP_LOGI(TAG, "ESP32-S3 cores=%d rev=%d flash=%u MB",
             chip.cores, chip.revision,
             static_cast<unsigned>(flash_size / (1024 * 1024)));

    ESP_ERROR_CHECK(g_settings.begin());
    ESP_ERROR_CHECK(i2c_bus_init());
    i2c_scan();
    run_peripheral_tests();

    // Reserve the adaptive ROM/cache arena before the display framebuffer,
    // DMA transfer buffer and background task stacks fragment internal RAM.
    ESP_ERROR_CHECK(start_audio());
    ESP_ERROR_CHECK(start_gameboy());
    ESP_ERROR_CHECK(start_graphics());
    ESP_ERROR_CHECK(start_haptics());
    ESP_ERROR_CHECK(start_input());

    const esp_err_t sd_err = start_storage();
    if (sd_err != ESP_OK) ESP_LOGW(TAG, "SD unavailable; WAV player will show instructions: %s", esp_err_to_name(sd_err));
    const esp_err_t imu_err = start_imu();
    if (imu_err != ESP_OK) ESP_LOGW(TAG, "BMI270 unavailable: %s", esp_err_to_name(imu_err));
    ESP_ERROR_CHECK(start_power());

    // Stable pogopoOS1.0 split: Core 1 runs high-priority input + emulator;
    // Core 0 runs high-priority I2S and low-priority GUI/Sharp presentation.
    // PDX startup executes the Lua loader on this task. Keep enough internal
    // stack for nested CoreLib imports while large directory-scan buffers live
    // in PSRAM (STEP11.6.18).
    if (xTaskCreatePinnedToCore(os_task, "pogopo_os", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "Could not create pogopoOS task");
    }

    start_system_tasks();
    ESP_LOGI(TAG, "STEP11.6.20 ready: CoreLibs object and scene classes");
}
