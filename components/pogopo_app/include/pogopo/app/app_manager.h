#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "pogopo/app/application.h"

namespace pogopo::app {

class AppManager {
public:
    // STEP9.5 already registers 12 built-ins. Keep inexpensive pointer slots
    // available for user games instead of silently rejecting the 13th app.
    static constexpr size_t MAX_APPS = 24;

    AppManager(gfx::Graphics& graphics, input::Input& input, haptics::Haptics& haptics,
               audio::Audio& audio, storage::Storage& storage, imu::Imu& imu, power::Power& power,
               settings::Settings& settings);

    bool registerApp(Application& app, bool home = false);
    bool start(const char* initial_id = nullptr);
    bool launch(const char* id);
    bool launch(size_t index);
    bool home();
    void toggleSystemMenu();
    void beginPowerOverlay();
    void endPowerOverlay();

    void processInput();
    void update(uint32_t dt_ms);
    esp_err_t render();

    void invalidate();
    void invalidate(const gfx::Rect& region);
    bool redrawPending() const { return full_redraw_ || !dirty_.empty(); }

    Application* activeApp() const { return active_; }
    const char* activeId() const { return active_ ? active_->id() : ""; }
    size_t appCount() const { return count_; }
    gui::Theme& theme() { return theme_; }
    Context& context() { return context_; }

    bool systemMenuOpen() const { return system_menu_open_; }
    bool powerOverlayOpen() const { return power_overlay_open_; }
    uint32_t renderedFrames() const { return rendered_frames_; }

private:
    Application* find(const char* id) const;
    bool switchTo(Application* app);
    void handleSystemMenu(const input::Event& event);
    void openSystemMenu();
    void closeSystemMenu(bool redraw_underlay = true, bool resume_app = true);
    void startSystemMenuClose(bool resume_app = true, const char* launch_target = nullptr);
    void finishSystemMenuClose();
    void adjustSystemVolume(int delta);
    void drawSystemMenu();
    int systemMenuItemCount() const;
    int systemMenuPanelX() const;
    gfx::Rect systemMenuRect() const;
    static gfx::Rect unite(const gfx::Rect& a, const gfx::Rect& b);

    gfx::Graphics& gfx_;
    input::Input& input_;
    haptics::Haptics& haptics_;
    audio::Audio& audio_;
    storage::Storage& storage_;
    imu::Imu& imu_;
    power::Power& power_;
    settings::Settings& settings_;
    gui::Theme theme_{};
    Context context_;

    std::array<Application*, MAX_APPS> apps_{};
    size_t count_ = 0;
    Application* active_ = nullptr;
    Application* home_ = nullptr;

    bool full_redraw_ = true;
    gfx::Rect dirty_{};
    bool system_menu_open_ = false;
    bool power_overlay_open_ = false;
    bool system_menu_closing_ = false;
    bool system_menu_resume_on_close_ = true;
    bool system_menu_underlay_valid_ = false;
    int system_menu_selected_ = 0;
    uint32_t system_menu_animation_ms_ = 0;
    uint32_t system_menu_redraw_ms_ = 0;
    const char* system_menu_launch_target_ = nullptr;
    std::array<uint8_t, gfx::SharpDisplay::FRAMEBUFFER_SIZE> system_menu_underlay_{};
    uint32_t rendered_frames_ = 0;
};

} // namespace pogopo::app
