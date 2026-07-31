#include "i2c_bus.h"
#include "peripheral_tests.h"
#include "tasks.h"
#include "board_pins.h"
#include "system_state.h"

#include "pogopo/gfx/gfx.h"
#include "pogopo_input.h"
#include "pogopo_haptics.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "app";
static const char* DEMO_TAG = "step4";

static pogopo::Graphics g_gfx;
static pogopo::Input g_input;
static pogopo::Haptics g_haptics;

// 16x16 transparent smile sprite, MSB-first, two bytes per row.
static constexpr uint8_t kPogopoSpriteData[] = {
    0x03,0xC0, 0x0F,0xF0, 0x1C,0x38, 0x30,0x0C,
    0x63,0xC6, 0x47,0xE2, 0xC6,0x63, 0xC0,0x03,
    0xC0,0x03, 0xC4,0x23, 0x66,0x66, 0x33,0xCC,
    0x1C,0x38, 0x0F,0xF0, 0x03,0xC0, 0x00,0x00,
};
static const pogopo::Bitmap kPogopoSprite =
    pogopo::gfx::make_bitmap_1bpp(16, 16, kPogopoSpriteData);

namespace {
constexpr pogopo::Rect kPlayfield{14, 58, 246, 104};
constexpr int kSpriteMinX = kPlayfield.x + 2;
constexpr int kSpriteMaxX = kPlayfield.x + kPlayfield.w - 18;
constexpr int kSpriteMinY = kPlayfield.y + 18;
constexpr int kSpriteMaxY = kPlayfield.y + kPlayfield.h - 18;

struct ButtonWidget {
    pogopo::Button button;
    int x;
    int y;
    const char* label;
};

constexpr ButtonWidget kButtonWidgets[] = {
    {pogopo::Button::Top,   300, 62,  "T"},
    {pogopo::Button::Left,  280, 82,  "L"},
    {pogopo::Button::Down,  300, 82,  "D"},
    {pogopo::Button::Right, 320, 82,  "R"},
    {pogopo::Button::B,     350, 76,  "B"},
    {pogopo::Button::A,     372, 66,  "A"},
    {pogopo::Button::Menu,  278, 116, "M"},
    {pogopo::Button::Start, 326, 116, "S"},
};

void draw_button_widget(const ButtonWidget& widget, bool pressed) {
    constexpr int w = 18;
    constexpr int h = 17;
    g_gfx.fillRect(widget.x, widget.y, w, h,
                   pressed ? pogopo::Graphics::BLACK : pogopo::Graphics::WHITE);
    g_gfx.drawRect(widget.x, widget.y, w, h, pogopo::Graphics::BLACK);
    g_gfx.drawText(widget.x + 6, widget.y + 5, widget.label,
                   pressed ? pogopo::Graphics::WHITE : pogopo::Graphics::BLACK, 1);
}

void draw_static_screen() {
    using G = pogopo::Graphics;

    g_gfx.clear(G::WHITE);
    g_gfx.drawRect(0, 0, g_gfx.width(), g_gfx.height(), G::BLACK);
    g_gfx.drawText(12, 10, "pogopoOS2.0 STEP4", G::BLACK, 2);
    g_gfx.drawText(12, 36, "pogopo::input + pogopo::haptics", G::BLACK, 1);

    g_gfx.drawRect(kPlayfield.x, kPlayfield.y, kPlayfield.w, kPlayfield.h, G::BLACK);
    g_gfx.drawText(kPlayfield.x + 6, kPlayfield.y + 5,
                   "D-PAD MOVES  A/B VIBRO", G::BLACK, 1);

    g_gfx.drawText(276, 48, "BUTTONS", G::BLACK, 1);
    for (const auto& widget : kButtonWidgets) {
        draw_button_widget(widget, false);
    }
    g_gfx.drawText(276, 140, "A+B: HEAVY", G::BLACK, 1);
    g_gfx.drawText(276, 152, "LONG M: ALERT", G::BLACK, 1);

    g_gfx.drawText(14, 174, "Event:", G::BLACK, 1);
    g_gfx.drawText(14, 188, "Held/Raw:", G::BLACK, 1);
    g_gfx.drawText(14, 202, "Input drop/err:", G::BLACK, 1);
    g_gfx.drawText(14, 216, "Vibro  FPS Rows Bytes:", G::BLACK, 1);

    ESP_ERROR_CHECK(g_gfx.presentFull());
}

void set_event_text(const pogopo::InputEvent& event, char* output, size_t output_size) {
    if (event.type == pogopo::InputEventType::Released ||
        event.type == pogopo::InputEventType::LongPress ||
        event.type == pogopo::InputEventType::Repeat) {
        std::snprintf(output, output_size, "%s %s %ums",
                      pogopo::event_type_name(event.type),
                      pogopo::button_name(event.button),
                      static_cast<unsigned>(event.held_ms));
    } else {
        std::snprintf(output, output_size, "%s %s",
                      pogopo::event_type_name(event.type),
                      pogopo::button_name(event.button));
    }
}

void input_haptics_demo_task(void*) {
    using G = pogopo::Graphics;

    int sprite_x = kPlayfield.x + kPlayfield.w / 2 - 8;
    int sprite_y = kPlayfield.y + kPlayfield.h / 2;
    int previous_x = -100; // force initial sprite draw
    int previous_y = -100;
    uint8_t previous_held = 0xFF; // force first redraw
    bool combo_latched = false;
    bool previous_haptic_active = false;
    bool status_dirty = true;
    char last_event[64] = "READY";

    uint32_t frames = 0;
    uint32_t shown_fps = 0;
    int64_t stats_start = esp_timer_get_time();
    TickType_t wake = xTaskGetTickCount();

    // Small boot acknowledgement; confirms the corrected GPIO3 transistor path.
    g_haptics.play(pogopo::HapticEffect::Tick);

    while (true) {
        pogopo::InputEvent event;
        while (g_input.nextEvent(event, 0)) {
            set_event_text(event, last_event, sizeof(last_event));
            status_dirty = true;
            ESP_LOGI(DEMO_TAG, "%s held=0x%02X raw=0x%02X",
                     last_event, event.held, g_input.rawPort());

            if (event.type == pogopo::InputEventType::Pressed) {
                switch (event.button) {
                    case pogopo::Button::A:
                        g_haptics.play(pogopo::HapticEffect::Click);
                        break;
                    case pogopo::Button::B:
                        g_haptics.play(pogopo::HapticEffect::DoubleClick);
                        break;
                    case pogopo::Button::Start:
                        sprite_x = kPlayfield.x + kPlayfield.w / 2 - 8;
                        sprite_y = kPlayfield.y + kPlayfield.h / 2;
                        g_haptics.play(pogopo::HapticEffect::Confirm);
                        break;
                    default:
                        break;
                }
            }

            if (event.type == pogopo::InputEventType::LongPress &&
                event.button == pogopo::Button::Menu) {
                g_haptics.play(pogopo::HapticEffect::Alert);
            }
        }

        const uint8_t held = g_input.heldMask();
        if (held & pogopo::mask(pogopo::Button::Left))  sprite_x -= 3;
        if (held & pogopo::mask(pogopo::Button::Right)) sprite_x += 3;
        if (held & pogopo::mask(pogopo::Button::Top))   sprite_y -= 3;
        if (held & pogopo::mask(pogopo::Button::Down))  sprite_y += 3;
        sprite_x = std::clamp(sprite_x, kSpriteMinX, kSpriteMaxX);
        sprite_y = std::clamp(sprite_y, kSpriteMinY, kSpriteMaxY);

        const pogopo::ButtonMask combo_mask = static_cast<pogopo::ButtonMask>(
            pogopo::mask(pogopo::Button::A) | pogopo::mask(pogopo::Button::B));
        const bool combo_now = g_input.comboHeld(combo_mask);
        if (combo_now && !combo_latched) {
            combo_latched = true;
            std::snprintf(last_event, sizeof(last_event), "COMBO A+B HEAVY");
            status_dirty = true;
            g_haptics.play(pogopo::HapticEffect::Heavy);
        } else if (!combo_now) {
            combo_latched = false;
        }

        if (sprite_x != previous_x || sprite_y != previous_y) {
            g_gfx.set_clip(kPlayfield);
            g_gfx.fillRect(previous_x, previous_y, 16, 16, G::WHITE);
            pogopo::Sprite sprite;
            sprite.bitmap = kPogopoSprite;
            sprite.x = sprite_x;
            sprite.y = sprite_y;
            sprite.foreground = G::BLACK;
            sprite.transparent_background = true;
            g_gfx.drawSprite(sprite);
            g_gfx.reset_clip();
            previous_x = sprite_x;
            previous_y = sprite_y;
        }

        if (held != previous_held) {
            for (const auto& widget : kButtonWidgets) {
                draw_button_widget(widget, (held & pogopo::mask(widget.button)) != 0);
            }
            previous_held = held;
            status_dirty = true;
        }

        const bool haptic_active = g_haptics.active();
        if (haptic_active != previous_haptic_active) {
            previous_haptic_active = haptic_active;
            status_dirty = true;
        }

        g_system_state.buttons_port.store(g_input.rawPort());
        g_system_state.buttons_ok.store(g_input.ok());

        ++frames;
        const int64_t now = esp_timer_get_time();
        if (now - stats_start >= 1000000) {
            shown_fps = frames;
            frames = 0;
            stats_start = now;
            status_dirty = true;
        }

        if (status_dirty) {
            const auto stats_before = g_gfx.stats();
            char value[128];

            g_gfx.fillRect(70, 170, 316, 59, G::WHITE);
            g_gfx.drawText(70, 174, last_event, G::BLACK, 1);

            std::snprintf(value, sizeof(value), "0x%02X / 0x%02X",
                          static_cast<unsigned>(held),
                          static_cast<unsigned>(g_input.rawPort()));
            g_gfx.drawText(92, 188, value, G::BLACK, 1);

            std::snprintf(value, sizeof(value), "%u / %u",
                          static_cast<unsigned>(g_input.droppedEvents()),
                          static_cast<unsigned>(g_input.readErrors()));
            g_gfx.drawText(112, 202, value, G::BLACK, 1);

            std::snprintf(value, sizeof(value), "%s  %u  %u  %u",
                          haptic_active ? "ON" : "OFF",
                          static_cast<unsigned>(shown_fps),
                          static_cast<unsigned>(stats_before.last_rows),
                          static_cast<unsigned>(stats_before.last_bytes));
            g_gfx.drawText(148, 216, value, G::BLACK, 1);
            status_dirty = false;
        }

        const esp_err_t present_err = g_gfx.present();
        if (present_err != ESP_OK) {
            ESP_LOGW(DEMO_TAG, "present failed: %s", esp_err_to_name(present_err));
        }

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(33));
    }
}

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

} // namespace

extern "C" void app_main(void) {
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(nullptr, &flash_size));

    ESP_LOGI(TAG, "pogopoOS2.0 INPUT + HAPTICS STEP4");
    ESP_LOGI(TAG, "ESP32-S3 cores=%d rev=%d flash=%u MB",
             chip.cores, chip.revision,
             static_cast<unsigned>(flash_size / (1024 * 1024)));

    ESP_ERROR_CHECK(i2c_bus_init());
    i2c_scan();
    run_peripheral_tests();

    ESP_ERROR_CHECK(start_graphics());
    ESP_ERROR_CHECK(start_haptics());
    ESP_ERROR_CHECK(start_input());

    draw_static_screen();
    g_gfx.resetStats();

    if (xTaskCreatePinnedToCore(input_haptics_demo_task,
                                "step4_demo",
                                6144,
                                nullptr,
                                3,
                                nullptr,
                                1) != pdPASS) {
        ESP_LOGE(TAG, "Could not create STEP4 demo task");
    }

    start_system_tasks();
    ESP_LOGI(TAG, "STEP4 ready: events + debounce + repeat + long press + queue + GPIO3 haptics");
}
