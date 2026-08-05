#pragma once

#include <cstdint>

#include "pogopo_app.h"
#include "pogopo_playdate.h"

namespace pogopo::demo {

class PogoDateApp final : public Application {
public:
    PogoDateApp(playdate::Game game, const char* app_id,
                const char* app_title, const char* game_title);

    const char* id() const override { return app_id_; }
    const char* title() const override { return app_title_; }

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
    playdate::Game game_ = playdate::Game::PDSnake;
    const char* app_id_ = "pogodate";
    const char* app_title_ = "PogoDate";
    const char* game_title_ = "PDSNAKE 1.2";
    esp_err_t start_error_ = ESP_ERR_INVALID_STATE;
    input::ButtonMask queued_pressed_ = 0;
    uint32_t lcd_accumulator_ms_ = 0;
    int64_t last_perf_us_ = 0;
    uint32_t previous_lua_frames_ = 0;
    uint32_t lcd_frames_ = 0;
    uint32_t previous_lcd_frames_ = 0;
    bool frame_pending_ = false;
};

} // namespace pogopo::demo
