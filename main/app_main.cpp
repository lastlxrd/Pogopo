#include "i2c_bus.h"
#include "peripheral_tests.h"
#include "tasks.h"
#include "board_pins.h"
#include "system_state.h"
#include "apps/demo_apps.h"
#include "apps/gameboy_apps.h"

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
#include "pogopo_startup.h"

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
static pogopo::demo::EmptyLibraryApp g_pogopo_library_app(
    "pogopo_library", "pogopo", "/pogopo/games", pogopo::menu::Art::Pogopo);
static pogopo::demo::EmptyLibraryApp g_playdate_library_app(
    "playdate_library", "playdate", "/playdate", pogopo::menu::Art::Playdate);
static pogopo::demo::GraphicsDemoApp g_graphics_app;
static pogopo::demo::InputMonitorApp g_input_app;
static pogopo::demo::HapticsLabApp g_haptics_app;
static pogopo::demo::AudioLabApp g_audio_app;
static pogopo::demo::WavPlayerApp g_wav_app;
static pogopo::demo::MotionLabApp g_motion_app;
static pogopo::demo::PowerStatusApp g_power_app;
static pogopo::demo::SettingsApp g_settings_app;
static pogopo::demo::PreferencesApp g_preferences_app;
static pogopo::demo::AboutApp g_about_app;
static pogopo::demo::GameBoyApp g_gameboy_app(g_gameboy);
static pogopo::demo::GameBoyBrowserApp g_gameboy_browser_app(g_gameboy_app);

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

    // Return the software framebuffer and the physical panel to white before
    // BATFET is disabled. The panel naturally relaxes to reflective white when
    // its rails collapse, so this also matches the first frame of the startup.
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

void discard_startup_input() {
    pogopo::input::Event event;
    while (g_input.nextEvent(event, 0)) {}
    g_input.consumePressedMask();
    g_input.consumeReleasedMask();
    g_input.consumeRepeatMask();
    g_input.consumeLongPressMask();
}

bool wait_startup_frame(TickType_t& next_frame,
                        bool allow_dismiss,
                        bool& input_armed,
                        bool& power_armed,
                        const char*& dismissed_by) {
    next_frame += pdMS_TO_TICKS(pogopo::StartupAnimation::FRAME_PERIOD_MS);

    while (true) {
        pogopo::input::Event input_event;
        while (g_input.nextEvent(input_event, 0)) {
            if (!allow_dismiss) continue;

            if (!input_armed) {
                if (g_input.heldMask() == 0) input_armed = true;
                continue;
            }
            if (input_event.type == pogopo::input::EventType::Pressed) {
                dismissed_by = pogopo::input::button_name(input_event.button);
                return true;
            }
        }

        if (allow_dismiss && !input_armed && g_input.heldMask() == 0) {
            input_armed = true;
        }

        pogopo::power::Event power_event;
        while (g_power.nextEvent(power_event, 0)) {
            if (power_event.type == pogopo::power::EventType::ShortPress) {
                if (allow_dismiss && power_armed) {
                    dismissed_by = "POWER";
                    return true;
                }
                continue;
            }

            handle_power_event(power_event);
            // USB-blocked handling can wait for release and draw its own card.
            // Restart the animation deadline instead of racing through frames.
            next_frame = xTaskGetTickCount() +
                         pdMS_TO_TICKS(pogopo::StartupAnimation::FRAME_PERIOD_MS);
        }

        if (allow_dismiss && !power_armed && !g_power.buttonDown()) {
            power_armed = true;
        }

        const TickType_t now = xTaskGetTickCount();
        const int32_t ticks_left = static_cast<int32_t>(next_frame - now);
        if (ticks_left <= 0) {
            // Never burst through several frames to catch up after a delayed
            // LCD transfer or a blocking power/USB notification.
            next_frame = now;
            return false;
        }

        const TickType_t poll_ticks = pdMS_TO_TICKS(4);
        vTaskDelay(std::min<TickType_t>(static_cast<TickType_t>(ticks_left),
                                       std::max<TickType_t>(poll_ticks, 1)));
    }
}

void play_startup_animation() {
    const pogopo::StartupAnimation animation;
    if (!animation.valid()) {
        ESP_LOGE(TAG, "Startup asset size mismatch: %u bytes, expected %u",
                 static_cast<unsigned>(animation.embeddedSize()),
                 static_cast<unsigned>(pogopo::StartupAnimation::FRAME_COUNT *
                                       pogopo::StartupAnimation::FRAME_SIZE));
        return;
    }

    ESP_LOGI(TAG, "Startup animation: %u frames at 10 FPS, loop=%u..%u, outro=%u..%u",
             static_cast<unsigned>(pogopo::StartupAnimation::FRAME_COUNT),
             static_cast<unsigned>(pogopo::StartupAnimation::LOOP_START + 1),
             static_cast<unsigned>(pogopo::StartupAnimation::LOOP_END),
             static_cast<unsigned>(pogopo::StartupAnimation::RESUME_START + 1),
             static_cast<unsigned>(pogopo::StartupAnimation::FRAME_COUNT));

    TickType_t next_frame = xTaskGetTickCount();
    bool input_armed = false;
    bool power_armed = false;
    const char* dismissed_by = nullptr;

    // Play frames 1..11 once. Inputs during this part are deliberately drained
    // so an old boot press cannot skip the interactive waiting animation.
    for (size_t frame = 0; frame < pogopo::StartupAnimation::LOOP_START; ++frame) {
        const esp_err_t error = animation.show(g_gfx, frame);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Startup frame %u failed: %s",
                     static_cast<unsigned>(frame + 1), esp_err_to_name(error));
            return;
        }
        wait_startup_frame(next_frame, false, input_armed, power_armed, dismissed_by);
    }

    discard_startup_input();
    input_armed = g_input.heldMask() == 0;
    power_armed = !g_power.buttonDown();

    // Frames 12..15 form the interactive idle loop. A fresh press exits only
    // this loop; the remaining outro still has to finish before the launcher.
    size_t frame = pogopo::StartupAnimation::LOOP_START;
    while (true) {
        const esp_err_t error = animation.show(g_gfx, frame);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Startup loop frame %u failed: %s",
                     static_cast<unsigned>(frame + 1), esp_err_to_name(error));
            return;
        }
        if (wait_startup_frame(next_frame, true, input_armed, power_armed, dismissed_by)) {
            discard_startup_input();
            ESP_LOGI(TAG, "Startup continued by %s",
                     dismissed_by ? dismissed_by : "BUTTON");
            break;
        }
        ++frame;
        if (frame >= pogopo::StartupAnimation::LOOP_END) {
            frame = pogopo::StartupAnimation::LOOP_START;
        }
    }

    // Start a fresh deadline so an early press in a loop frame cannot shorten
    // frame 16. Frames 16..25 then play exactly once before the menu appears.
    next_frame = xTaskGetTickCount();
    for (frame = pogopo::StartupAnimation::RESUME_START;
         frame < pogopo::StartupAnimation::FRAME_COUNT;
         ++frame) {
        const esp_err_t error = animation.show(g_gfx, frame);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Startup outro frame %u failed: %s",
                     static_cast<unsigned>(frame + 1), esp_err_to_name(error));
            return;
        }
        wait_startup_frame(next_frame, false, input_armed, power_armed, dismissed_by);
    }

    // Do not leak the continue press or any presses made during the outro into
    // the launcher that is about to become active.
    discard_startup_input();
    ESP_LOGI(TAG, "Startup outro complete");
}

void os_task(void*) {
    int64_t last_us = esp_timer_get_time();
    TickType_t wake = xTaskGetTickCount();
    uint32_t settings_dirty_ms = 0;

    g_app_manager.registerApp(g_launcher_app, true);
    g_app_manager.registerApp(g_pogopo_library_app);
    g_app_manager.registerApp(g_playdate_library_app);
    g_app_manager.registerApp(g_gameboy_browser_app);
    g_app_manager.registerApp(g_gameboy_app);
    g_app_manager.registerApp(g_graphics_app);
    g_app_manager.registerApp(g_input_app);
    g_app_manager.registerApp(g_haptics_app);
    g_app_manager.registerApp(g_audio_app);
    g_app_manager.registerApp(g_wav_app);
    g_app_manager.registerApp(g_motion_app);
    g_app_manager.registerApp(g_power_app);
    g_app_manager.registerApp(g_settings_app);
    g_app_manager.registerApp(g_preferences_app);
    g_app_manager.registerApp(g_about_app);

    if (!pogopo::menu::Assets::valid()) {
        ESP_LOGE(TAG, "Menu asset size mismatch: %u bytes",
                 static_cast<unsigned>(pogopo::menu::Assets::embeddedSize()));
    } else {
        ESP_LOGI(TAG, "STEP13.1 menu assets ready: %u bytes",
                 static_cast<unsigned>(pogopo::menu::Assets::embeddedSize()));
    }

    play_startup_animation();
    g_app_manager.start("launcher");
    g_haptics.play(pogopo::HapticEffect::Confirm);
    if (g_settings.uiSoundsEnabled()) g_audio.play(pogopo::AudioEffect::Startup);
    ESP_LOGI(TAG, "STEP13.1 polished launcher ready after startup");

    // The startup can wait in its 12..15 loop indefinitely. Reset both OS
    // clocks so the first menu frame begins at animation time zero instead of
    // consuming the 100 ms dt clamp before it is ever drawn.
    last_us = esp_timer_get_time();
    wake = xTaskGetTickCount();

    while (true) {
        const int64_t now_us = esp_timer_get_time();
        const uint32_t dt_ms = static_cast<uint32_t>(std::clamp<int64_t>((now_us - last_us) / 1000, 0, 100));
        last_us = now_us;

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
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(8));
    }
}

} // namespace

extern "C" void app_main(void) {
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(nullptr, &flash_size));

    ESP_LOGI(TAG, "pogopoOS2.0 STEP13.1 MENU POLISH");
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
    if (xTaskCreatePinnedToCore(os_task, "pogopo_os", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "Could not create pogopoOS task");
    }

    start_system_tasks();
    ESP_LOGI(TAG, "STEP13.1 system tasks started: startup animation pending");
}
