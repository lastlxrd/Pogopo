#pragma once

#include <cstdint>
#include <array>

#include "pogopo_app.h"
#include "pogopo_gui.h"
#include "pogopo_playdate.h"

namespace pogopo::demo {

class PogoDateApp final : public Application {
public:
    PogoDateApp(playdate::Game game, const char* app_id,
                const char* app_title, const char* game_title);
    void preparePackage(const playdate::PackageInfo& package);

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
    const char* displayTitle() const;

    playdate::Runtime runtime_{};
    playdate::Game game_ = playdate::Game::PDSnake;
    const char* app_id_ = "pogodate";
    const char* app_title_ = "PogoDate";
    const char* game_title_ = "PDSNAKE 1.2";
    playdate::PackageInfo package_{};
    esp_err_t start_error_ = ESP_ERR_INVALID_STATE;
    input::ButtonMask queued_pressed_ = 0;
    uint32_t lcd_accumulator_ms_ = 0;
    int64_t last_perf_us_ = 0;
    uint32_t previous_lua_frames_ = 0;
    uint32_t lcd_frames_ = 0;
    uint32_t previous_lcd_frames_ = 0;
    bool frame_pending_ = false;
    uint32_t accelerometer_sequence_ = 0;
    float accelerometer_x_ = 0.0f;
    float accelerometer_y_ = 0.0f;
    float accelerometer_z_ = 1.0f;
    bool accelerometer_initialized_ = false;
};

class PogoDateBrowserApp final : public Application {
public:
    explicit PogoDateBrowserApp(PogoDateApp& player) : player_(player) {}
    // The STEP13 home-screen Playdate tile launches this stable app id.
    const char* id() const override { return "playdate_library"; }
    const char* title() const override { return "Playdate SD"; }
    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void update(AppContext& context, uint32_t dt_ms) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;

private:
    void rescan(AppContext& context);
    void launchSelected(AppContext& context);

    PogoDateApp& player_;
    gui::List list_{{18, 42, 364, 148}};
    std::array<playdate::PackageInfo, storage::Storage::MAX_FILES> packages_{};
    std::array<gui::ListItem, storage::Storage::MAX_FILES> items_{};
    std::array<std::array<char, 48>, storage::Storage::MAX_FILES> subtitles_{};
    size_t package_count_ = 0;
    char status_[96] = "READY";
    uint32_t enter_elapsed_ms_ = 0;
    uint32_t redraw_elapsed_ms_ = 0;
};

} // namespace pogopo::demo
