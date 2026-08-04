#pragma once

#include <cstdint>

#include "pogopo_app.h"
#include "pogopo_playdate.h"

namespace pogopo::demo {

class PogoDateApp final : public Application {
public:
    const char* id() const override { return "pogodate"; }
    const char* title() const override { return "PogoDate"; }

    void onEnter(AppContext& context) override;
    void onExit(AppContext& context) override;
    void onSuspend(AppContext& context) override;
    void onResume(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void update(AppContext& context, uint32_t dt_ms) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;

private:
    void drawLoading(AppContext& context);

    playdate::Runtime runtime_{};
    esp_err_t start_error_ = ESP_ERR_INVALID_STATE;
    input::ButtonMask queued_pressed_ = 0;
    uint32_t lcd_accumulator_ms_ = 0;
    uint32_t perf_accumulator_ms_ = 0;
    uint32_t previous_lua_frames_ = 0;
    uint32_t lcd_frames_ = 0;
    uint32_t previous_lcd_frames_ = 0;
    bool frame_pending_ = false;
};

} // namespace pogopo::demo
