#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "pogopo_app.h"
#include "pogopo_gui.h"
#include "pogopo_menu.h"

namespace pogopo::demo {

class LauncherApp final : public Application {
public:
    LauncherApp();
    const char* id() const override { return "launcher"; }
    const char* title() const override { return "Launcher"; }
    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void update(AppContext& context, uint32_t dt_ms) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;

private:
    enum class Phase : uint8_t { Entering, Idle, Switching, Opening };
    void startSwitch(AppContext& context, int direction);
    void drawArt(AppContext& context, int item, int y_offset, uint32_t elapsed_ms);
    void drawSettingsArt(AppContext& context, int y_offset, uint32_t elapsed_ms);
    void drawGameBoyRoll(AppContext& context, float progress, bool entering);

    int selected_ = 0;
    int previous_ = 0;
    int direction_ = 1;
    Phase phase_ = Phase::Entering;
    uint32_t phase_elapsed_ms_ = 0;
    uint32_t art_elapsed_ms_ = 0;
    uint32_t redraw_elapsed_ms_ = 0;
    uint16_t last_art_key_ = UINT16_MAX;
    uint8_t last_battery_key_ = 0xFF;
    const char* launch_target_ = nullptr;
};

class EmptyLibraryApp final : public Application {
public:
    EmptyLibraryApp(const char* app_id, const char* platform_name,
                    const char* folder, menu::Art art)
        : app_id_(app_id), platform_name_(platform_name), folder_(folder), art_(art) {}
    const char* id() const override { return app_id_; }
    const char* title() const override { return platform_name_; }
    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void update(AppContext& context, uint32_t dt_ms) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;
private:
    const char* app_id_;
    const char* platform_name_;
    const char* folder_;
    menu::Art art_;
    uint32_t elapsed_ms_ = 0;
    uint32_t redraw_elapsed_ms_ = 0;
    uint32_t enter_elapsed_ms_ = 0;
    size_t last_frame_ = static_cast<size_t>(-1);
};

class GraphicsDemoApp final : public Application {
public:
    const char* id() const override { return "graphics"; }
    const char* title() const override { return "Graphics"; }
    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void update(AppContext& context, uint32_t dt_ms) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;

private:
    int x_ = 35;
    int previous_x_ = 35;
    int velocity_ = 3;
    uint32_t accumulator_ms_ = 0;
    bool paused_ = false;
    uint32_t frame_counter_ = 0;
};

class InputMonitorApp final : public Application {
public:
    const char* id() const override { return "input"; }
    const char* title() const override { return "Input"; }
    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void update(AppContext& context, uint32_t dt_ms) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;

private:
    input::ButtonMask last_held_ = 0xFF;
    uint8_t last_raw_ = 0xFF;
};

class HapticsLabApp final : public Application {
public:
    HapticsLabApp();
    const char* id() const override { return "haptics"; }
    const char* title() const override { return "Haptics"; }
    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;

private:
    gui::List list_{{22, 48, 356, 147}};
    const char* last_effect_ = "READY";
};


class AudioLabApp final : public Application {
public:
    AudioLabApp();
    const char* id() const override { return "audio"; }
    const char* title() const override { return "Audio"; }
    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void update(AppContext& context, uint32_t dt_ms) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;

private:
    gui::List list_{{22, 43, 356, 133}};
    const char* last_sound_ = "READY";
    uint32_t stats_accumulator_ms_ = 0;
    uint32_t last_buffers_ = 0;
    uint8_t last_voices_ = 0;
};


class WavPlayerApp final : public Application {
public:
    const char* id() const override { return "wav"; }
    const char* title() const override { return "WAV Player"; }
    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void update(AppContext& context, uint32_t dt_ms) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;
private:
    void rescan(AppContext& context);
    void startSelected(AppContext& context);
    gui::List list_{{18, 42, 364, 116}};
    std::array<storage::FileEntry, storage::Storage::MAX_FILES> files_{};
    std::array<gui::ListItem, storage::Storage::MAX_FILES> items_{};
    std::array<std::array<char, 32>, storage::Storage::MAX_FILES> subtitles_{};
    size_t file_count_ = 0;
    uint32_t ui_accumulator_ms_ = 0;
    audio::StreamState last_state_ = audio::StreamState::Stopped;
    uint32_t last_position_second_ = 0;
    char status_[80] = "READY";
};

class SettingsApp final : public Application {
public:
    const char* id() const override { return "settings"; }
    const char* title() const override { return "Settings"; }
    void onEnter(AppContext& context) override;
    void onExit(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void update(AppContext& context, uint32_t dt_ms) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;
private:
    int selected_ = 0;
    uint32_t enter_elapsed_ms_ = 0;
    uint32_t status_elapsed_ms_ = 0;
};

class PreferencesApp final : public Application {
public:
    const char* id() const override { return "preferences"; }
    const char* title() const override { return "Preferences"; }
    void onEnter(AppContext& context) override;
    void onExit(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void update(AppContext& context, uint32_t dt_ms) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;
private:
    void applyRuntime(AppContext& context);
    void markChanged(AppContext& context);
    int selected_ = 0;
    uint32_t save_delay_ms_ = 0;
    char status_[48] = "NVS READY";
};

class MotionLabApp final : public Application {
public:
    const char* id() const override { return "motion"; }
    const char* title() const override { return "Motion"; }
    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void update(AppContext& context, uint32_t dt_ms) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;
private:
    uint32_t last_sequence_ = 0;
    float zero_roll_ = 0;
    float zero_pitch_ = 0;
    float visual_roll_ = 0;
    float visual_pitch_ = 0;
    bool visual_initialized_ = false;
    imu::Sample latest_{};
};

class PowerStatusApp final : public Application {
public:
    const char* id() const override { return "power"; }
    const char* title() const override { return "Power"; }
    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void update(AppContext& context, uint32_t dt_ms) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;
private:
    uint32_t last_sequence_ = 0;
    power::State latest_{};
};

class AboutApp final : public Application {
public:
    const char* id() const override { return "about"; }
    const char* title() const override { return "About"; }
    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;
};

} // namespace pogopo::demo
