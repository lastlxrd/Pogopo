#include "apps/playdate_apps.h"

#include <algorithm>
#include <cstdio>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace pogopo::demo {
namespace {
constexpr char TAG[] = "pogodate_app";
constexpr uint32_t LCD_FRAME_MS = 33;
}

void PogoDateApp::drawLoading(AppContext& context) {
    auto& canvas = context.gfx.canvas();
    context.gfx.reset_clip();
    canvas.clear(context.theme.background);
    gui::draw_header(canvas, context.theme, "POGODATE LITE", "LOADING");
    canvas.draw_rect(24, 49, 352, 139, context.theme.border);
    canvas.draw_text(43, 70, "PLAYDATE LUA COMPATIBILITY TEST",
                     gfx::font5x7(), context.theme.foreground);
    canvas.draw_text(43, 99, "GAME: PDSNAKE 1.2",
                     gfx::font5x7(), context.theme.foreground, 2);
    canvas.draw_text(43, 134, "LUA 5.4 + NATIVE POGOPO API",
                     gfx::font5x7(), context.theme.foreground);
    canvas.draw_text(43, 157, "400 x 240 / 1-BIT / NO SCALING",
                     gfx::font5x7(), context.theme.foreground);
    gui::draw_footer(canvas, context.theme, "PLEASE WAIT", "SOURCE LUA");
    context.gfx.presentFull();
}

void PogoDateApp::onEnter(AppContext& context) {
    queued_pressed_ = 0;
    lcd_accumulator_ms_ = 0;
    perf_accumulator_ms_ = 0;
    previous_lua_frames_ = 0;
    lcd_frames_ = 0;
    previous_lcd_frames_ = 0;
    frame_pending_ = false;

    context.audio.stopStream();
    context.audio.stopRealtime();
    drawLoading(context);
    start_error_ = runtime_.start(context.gfx.canvas(), context.audio,
                                  context.storage);
    if (start_error_ == ESP_OK) {
        context.haptics.play(haptics::Effect::Confirm);
        ESP_LOGI(TAG, "PDSnake started from original Lua sources");
    } else {
        context.haptics.play(haptics::Effect::Alert);
        context.uiSound(audio::Effect::Error);
        ESP_LOGE(TAG, "PogoDate start failed: %s / %s",
                 esp_err_to_name(start_error_), runtime_.error());
    }
    context.invalidate();
}

void PogoDateApp::onExit(AppContext&) {
    runtime_.stop();
    queued_pressed_ = 0;
    frame_pending_ = false;
}

void PogoDateApp::onSuspend(AppContext&) {
    queued_pressed_ = 0;
    runtime_.setInput(0, 0);
}

void PogoDateApp::onResume(AppContext& context) {
    lcd_accumulator_ms_ = LCD_FRAME_MS;
    frame_pending_ = true;
    context.invalidate();
}

void PogoDateApp::onEvent(AppContext&, const input::Event& event) {
    if (event.type == input::EventType::Pressed) {
        queued_pressed_ = static_cast<input::ButtonMask>(
            queued_pressed_ | input::mask(event.button));
    }
}

void PogoDateApp::update(AppContext& context, uint32_t dt_ms) {
    if (start_error_ != ESP_OK || !runtime_.running()) return;

    runtime_.setInput(context.input.heldMask(), queued_pressed_);
    const uint32_t produced = runtime_.update(dt_ms);
    if (produced > 0) {
        queued_pressed_ = 0;
        frame_pending_ = true;
    }

    if (!runtime_.running()) {
        start_error_ = ESP_FAIL;
        context.haptics.play(haptics::Effect::Alert);
        context.uiSound(audio::Effect::Error);
        context.invalidate();
        return;
    }

    lcd_accumulator_ms_ =
        std::min<uint32_t>(lcd_accumulator_ms_ + dt_ms, 100U);
    if (frame_pending_ && lcd_accumulator_ms_ >= LCD_FRAME_MS) {
        lcd_accumulator_ms_ -= LCD_FRAME_MS;
        frame_pending_ = false;
        ++lcd_frames_;
        context.invalidate();
    }

    perf_accumulator_ms_ += dt_ms;
    if (perf_accumulator_ms_ >= 1000U) {
        const playdate::Stats stats = runtime_.stats();
        const uint32_t lua_delta =
            stats.lua_frames - previous_lua_frames_;
        const uint32_t lcd_delta =
            lcd_frames_ - previous_lcd_frames_;
        ESP_LOGI(
            TAG,
            "PERF PD lua=%lu lcd=%lu target=%lu update=%luus max=%luus "
            "luaheap=%lu peak=%lu gc=%lu err=%lu RAM=%lu/%lu PSRAM=%lu "
            "i2cerr=%lu",
            static_cast<unsigned long>(lua_delta),
            static_cast<unsigned long>(lcd_delta),
            static_cast<unsigned long>(stats.requested_fps),
            static_cast<unsigned long>(stats.last_update_us),
            static_cast<unsigned long>(stats.max_update_us),
            static_cast<unsigned long>(stats.lua_bytes),
            static_cast<unsigned long>(stats.lua_peak_bytes),
            static_cast<unsigned long>(stats.lua_gc_bytes),
            static_cast<unsigned long>(stats.errors),
            static_cast<unsigned long>(heap_caps_get_free_size(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned long>(heap_caps_get_largest_free_block(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned long>(heap_caps_get_free_size(
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
            static_cast<unsigned long>(context.input.readErrors()));
        previous_lua_frames_ = stats.lua_frames;
        previous_lcd_frames_ = lcd_frames_;
        perf_accumulator_ms_ = 0;
    }
}

void PogoDateApp::draw(AppContext& context, const gfx::Rect&) {
    if (start_error_ == ESP_OK && runtime_.running()) {
        // The Lua graphics API draws into the native Sharp framebuffer during
        // update(). AppManager owns only the paced physical presentation.
        return;
    }

    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(context.theme.background);
    gui::draw_header(canvas, context.theme, "POGODATE LITE", "LUA ERROR");
    canvas.draw_rect(22, 47, 356, 150, context.theme.border);
    canvas.draw_text(39, 67, "PDSNAKE COULD NOT CONTINUE",
                     gfx::font5x7(), context.theme.foreground, 2);
    char status[96]{};
    std::snprintf(status, sizeof(status), "%s", runtime_.error());
    canvas.draw_text(39, 111, status, gfx::font5x7(),
                     context.theme.foreground);
    canvas.draw_text(39, 151, "CHECK SERIAL FOR FULL DIAGNOSTIC",
                     gfx::font5x7(), context.theme.foreground);
    gui::draw_footer(canvas, context.theme, "POWER MENU -> HOME",
                     esp_err_to_name(start_error_));
}

} // namespace pogopo::demo
