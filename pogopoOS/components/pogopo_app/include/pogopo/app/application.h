#pragma once

#include <cstddef>
#include <cstdint>

#include "pogopo/gfx/gfx.h"
#include "pogopo_input.h"
#include "pogopo_haptics.h"
#include "pogopo_audio.h"
#include "pogopo_gui.h"
#include "pogopo_storage.h"
#include "pogopo_imu.h"
#include "pogopo_power.h"
#include "pogopo_settings.h"

namespace pogopo::app {

class AppManager;

class Context {
public:
    Context(gfx::Graphics& graphics, input::Input& input, haptics::Haptics& haptics,
            audio::Audio& audio, storage::Storage& storage, imu::Imu& imu, power::Power& power,
            settings::Settings& settings, gui::Theme& theme, AppManager& manager)
        : gfx(graphics), input(input), haptics(haptics), audio(audio),
          storage(storage), imu(imu), power(power), settings(settings), theme(theme), manager(manager) {}

    void invalidate();
    void invalidate(const gfx::Rect& region);
    bool launch(const char* id);
    bool home();
    bool uiSound(audio::Effect effect);

    gfx::Graphics& gfx;
    input::Input& input;
    haptics::Haptics& haptics;
    audio::Audio& audio;
    storage::Storage& storage;
    imu::Imu& imu;
    power::Power& power;
    settings::Settings& settings;
    gui::Theme& theme;
    AppManager& manager;
};

class Application {
public:
    virtual ~Application() = default;
    virtual const char* id() const = 0;
    virtual const char* title() const = 0;
    // Emulators may reserve MENU as a game button (for example GB Select).
    virtual bool capturesMenuButton() const { return false; }
    // App-owned actions are placed at the top of the Power quick menu. The
    // manager supplies Resume and the system actions around them.
    virtual size_t quickActionCount() const { return 0; }
    virtual const char* quickActionLabel(size_t /*index*/) const { return ""; }
    virtual bool runQuickAction(Context&, size_t /*index*/) { return false; }

    virtual void onEnter(Context&) {}
    virtual void onExit(Context&) {}
    // Called while a system overlay owns input. Games with background tasks
    // should pause them here and resume them in onResume().
    virtual void onSuspend(Context&) {}
    virtual void onResume(Context&) {}
    virtual void onEvent(Context&, const input::Event&) {}
    virtual void update(Context&, uint32_t /*dt_ms*/) {}
    virtual void draw(Context&, const gfx::Rect& dirty_region) = 0;
};

} // namespace pogopo::app
