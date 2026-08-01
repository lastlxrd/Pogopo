#include "pogopo/app/app_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pogopo::app {

namespace {
constexpr int kSystemMenuItemCount = 4;
constexpr int kResumeItem = 0;
constexpr int kVolumeItem = 1;
constexpr int kSettingsItem = 2;
constexpr int kHomeItem = 3;
}

void Context::invalidate() { manager.invalidate(); }
void Context::invalidate(const gfx::Rect& region) { manager.invalidate(region); }
bool Context::launch(const char* id) { return manager.launch(id); }
bool Context::home() { return manager.home(); }
bool Context::uiSound(audio::Effect effect) {
    return settings.uiSoundsEnabled() ? audio.play(effect) : true;
}

AppManager::AppManager(gfx::Graphics& graphics, input::Input& input,
                       haptics::Haptics& haptics, audio::Audio& audio,
                       storage::Storage& storage, imu::Imu& imu, power::Power& power,
                       settings::Settings& settings)
    : gfx_(graphics), input_(input), haptics_(haptics), audio_(audio),
      storage_(storage), imu_(imu), power_(power), settings_(settings),
      context_(graphics, input, haptics, audio, storage, imu, power, settings, theme_, *this) {}

bool AppManager::registerApp(Application& app, bool home) {
    if (count_ >= apps_.size() || find(app.id())) return false;
    apps_[count_++] = &app;
    if (home || !home_) home_ = &app;
    return true;
}

bool AppManager::start(const char* initial_id) {
    Application* target = initial_id ? find(initial_id) : home_;
    if (!target && count_ > 0) target = apps_[0];
    return switchTo(target);
}

Application* AppManager::find(const char* id) const {
    if (!id) return nullptr;
    for (size_t i = 0; i < count_; ++i) {
        if (apps_[i] && std::strcmp(apps_[i]->id(), id) == 0) return apps_[i];
    }
    return nullptr;
}

bool AppManager::switchTo(Application* app) {
    if (!app) return false;
    // Leaving from the overlay should not briefly resume a background game
    // task only to stop it again in onExit().
    closeSystemMenu(false, false);
    if (active_ == app) {
        invalidate();
        return true;
    }
    if (active_) active_->onExit(context_);
    active_ = app;
    active_->onEnter(context_);
    invalidate();
    return true;
}

bool AppManager::launch(const char* id) { return switchTo(find(id)); }
bool AppManager::launch(size_t index) { return index < count_ ? switchTo(apps_[index]) : false; }
bool AppManager::home() { return switchTo(home_); }

void AppManager::toggleSystemMenu() {
    if (system_menu_open_) {
        haptics_.play(haptics::Effect::Click);
        if (settings_.uiSoundsEnabled()) audio_.play(audio::Effect::Back);
        closeSystemMenu(true);
    } else {
        openSystemMenu();
    }
}

void AppManager::processInput() {
    input::Event event;
    while (input_.nextEvent(event, 0)) {
        if (system_menu_open_) {
            handleSystemMenu(event);
            continue;
        }
        if (event.type == input::EventType::Pressed && event.button == input::Button::Menu &&
            (!active_ || !active_->capturesMenuButton())) {
            openSystemMenu();
            continue;
        }
        if (active_) active_->onEvent(context_, event);
    }
}

void AppManager::update(uint32_t dt_ms) {
    if (active_ && !system_menu_open_) active_->update(context_, dt_ms);
}

gfx::Rect AppManager::unite(const gfx::Rect& a, const gfx::Rect& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const int x = std::min(a.x, b.x);
    const int y = std::min(a.y, b.y);
    const int right = std::max(a.right(), b.right());
    const int bottom = std::max(a.bottom(), b.bottom());
    return {x, y, right - x, bottom - y};
}

void AppManager::invalidate() {
    full_redraw_ = true;
    dirty_ = {0, 0, gfx_.width(), gfx_.height()};
}

void AppManager::invalidate(const gfx::Rect& region) {
    if (full_redraw_) return;
    const gfx::Rect clipped = gfx::Rect::intersect(region, {0, 0, gfx_.width(), gfx_.height()});
    dirty_ = unite(dirty_, clipped);
}

esp_err_t AppManager::render() {
    if (!active_ || !redrawPending()) return ESP_OK;
    const gfx::Rect region = full_redraw_ ? gfx::Rect{0, 0, gfx_.width(), gfx_.height()} : dirty_;
    if (region.empty()) return ESP_OK;

    gfx_.set_clip(region);
    active_->draw(context_, region);
    if (system_menu_open_) drawSystemMenu();
    gfx_.reset_clip();

    full_redraw_ = false;
    dirty_ = {};
    ++rendered_frames_;
    return gfx_.present();
}

gfx::Rect AppManager::systemMenuRect() const {
    return {62, 16, 276, 208};
}

void AppManager::openSystemMenu() {
    if (system_menu_open_) return;
    system_menu_open_ = true;
    system_menu_selected_ = 0;
    if (active_) active_->onSuspend(context_);
    haptics_.play(haptics::Effect::DoubleClick);
    if (settings_.uiSoundsEnabled()) audio_.play(audio::Effect::Click);
    invalidate(systemMenuRect());
}

void AppManager::closeSystemMenu(bool redraw_underlay, bool resume_app) {
    if (!system_menu_open_) return;
    system_menu_open_ = false;
    if (resume_app && active_) active_->onResume(context_);
    if (redraw_underlay) invalidate();
}

void AppManager::adjustSystemVolume(int delta) {
    const int volume = std::clamp<int>(
        static_cast<int>(audio_.masterVolume()) + delta, 0, 100);
    audio_.setMasterVolume(static_cast<uint8_t>(volume));
    settings_.setVolume(static_cast<uint8_t>(volume));
    haptics_.play(haptics::Effect::Tick);
    if (settings_.uiSoundsEnabled()) audio_.play(audio::Effect::Tick);
    invalidate(systemMenuRect());
}

void AppManager::handleSystemMenu(const input::Event& event) {
    const bool navigation = event.type == input::EventType::Pressed ||
                            event.type == input::EventType::Repeat;
    if (!navigation) return;

    if (event.button == input::Button::Top) {
        system_menu_selected_ =
            (system_menu_selected_ + kSystemMenuItemCount - 1) % kSystemMenuItemCount;
        haptics_.play(haptics::Effect::Tick);
        if (settings_.uiSoundsEnabled()) audio_.play(audio::Effect::Tick);
        invalidate(systemMenuRect());
    } else if (event.button == input::Button::Down) {
        system_menu_selected_ = (system_menu_selected_ + 1) % kSystemMenuItemCount;
        haptics_.play(haptics::Effect::Tick);
        if (settings_.uiSoundsEnabled()) audio_.play(audio::Effect::Tick);
        invalidate(systemMenuRect());
    } else if (system_menu_selected_ == kVolumeItem &&
               event.button == input::Button::Left) {
        adjustSystemVolume(-5);
    } else if (system_menu_selected_ == kVolumeItem &&
               event.button == input::Button::Right) {
        adjustSystemVolume(5);
    } else if (event.type == input::EventType::Pressed &&
               (event.button == input::Button::B || event.button == input::Button::Menu)) {
        haptics_.play(haptics::Effect::Click);
        if (settings_.uiSoundsEnabled()) audio_.play(audio::Effect::Back);
        closeSystemMenu(true);
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::A) {
        const int action = system_menu_selected_;
        if (action == kVolumeItem) {
            adjustSystemVolume(5);
            return;
        }
        haptics_.play(haptics::Effect::Confirm);
        if (settings_.uiSoundsEnabled()) audio_.play(audio::Effect::Confirm);
        if (action == kResumeItem) {
            closeSystemMenu(true);
        } else if (action == kSettingsItem) {
            closeSystemMenu(false, false);
            launch("settings");
        } else if (action == kHomeItem) {
            closeSystemMenu(false, false);
            home();
        }
    }
}

void AppManager::drawSystemMenu() {
    char volume[32]{};
    std::snprintf(volume, sizeof(volume), "%u%%  LEFT/RIGHT",
                  static_cast<unsigned>(audio_.masterVolume()));
    const gui::ListItem items[kSystemMenuItemCount] = {
        {"Resume", "Return to game or app", "resume", true},
        {"Volume", volume, "volume", true},
        {"Settings", "Open system settings", "settings", true},
        {"Home", "Leave app and open launcher", "home", true},
    };

    gui::Dialog dialog(systemMenuRect());
    dialog.setTitle("QUICK MENU");
    dialog.setMessage("UP/DOWN SELECT");
    dialog.setFooter("A SELECT   B BACK   L/R VOLUME");
    dialog.draw(gfx_.canvas(), theme_);

    gui::List list({systemMenuRect().x + 14, systemMenuRect().y + 54,
                    systemMenuRect().w - 28, 116});
    list.setItems(items, kSystemMenuItemCount);
    list.setRowHeight(28);
    list.setSelected(static_cast<size_t>(system_menu_selected_));
    list.draw(gfx_.canvas(), theme_);
}

} // namespace pogopo::app
