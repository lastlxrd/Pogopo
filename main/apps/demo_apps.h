#pragma once

#include <cstdint>

#include "pogopo_app.h"
#include "pogopo_gui.h"

namespace pogopo::demo {

class LauncherApp final : public Application {
public:
    LauncherApp();
    const char* id() const override { return "launcher"; }
    const char* title() const override { return "Launcher"; }
    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;

private:
    gui::List list_{{20, 43, 360, 164}};
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

class AboutApp final : public Application {
public:
    const char* id() const override { return "about"; }
    const char* title() const override { return "About"; }
    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;
};

} // namespace pogopo::demo
