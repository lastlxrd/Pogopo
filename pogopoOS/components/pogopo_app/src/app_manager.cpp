#include "pogopo/app/app_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "pogopo_menu.h"

namespace pogopo::app {

namespace {
constexpr uint32_t kSystemMenuDurationMs = 180;
constexpr uint32_t kSystemMenuFrameMs = 32;
constexpr int kSystemMenuLeft = 202;
constexpr int kSystemMenuWidth = 198;
constexpr int kSystemMenuRowY = 7;
constexpr int kSystemMenuRowHeight = 31;
constexpr int kResumeItem = 0;

float smooth_step(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

int interpolate(int from, int to, float progress) {
    return static_cast<int>(std::lround(
        static_cast<float>(from) + static_cast<float>(to - from) * progress));
}

void fill_pill(gfx::Canvas& canvas, int x, int y, int width, int height,
               gfx::Color color) {
    if (width <= 0 || height <= 0) return;
    const int radius = std::min(height / 2, width / 2);
    canvas.fill_rect(x + radius, y, width - radius * 2, height, color);
    canvas.fill_circle(x + radius, y + height / 2, radius, color);
    canvas.fill_circle(x + width - radius - 1, y + height / 2, radius, color);
}
} // namespace

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
    if (power_overlay_open_) return;
    if (system_menu_open_) {
        if (!system_menu_closing_) {
            haptics_.play(haptics::Effect::Click);
            if (settings_.uiSoundsEnabled()) audio_.play(audio::Effect::Back);
            startSystemMenuClose(true);
        }
    } else {
        openSystemMenu();
    }
}

void AppManager::beginPowerOverlay() {
    if (power_overlay_open_) return;
    // A long Power hold owns the whole display. If the quick panel was open,
    // restore its captured underlay first so Outro never bakes the panel into
    // the frame that will be restored after a cancelled shutdown.
    const bool already_suspended = system_menu_open_;
    if (system_menu_open_) closeSystemMenu(false, false);
    power_overlay_open_ = true;
    if (active_ && !already_suspended) active_->onSuspend(context_);
    full_redraw_ = false;
    dirty_ = {};
}

void AppManager::endPowerOverlay() {
    if (!power_overlay_open_) return;
    power_overlay_open_ = false;
    if (active_) active_->onResume(context_);
    invalidate();
}

void AppManager::processInput() {
    if (power_overlay_open_) return;
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
    if (power_overlay_open_) return;
    if (!system_menu_open_) {
        if (active_) active_->update(context_, dt_ms);
        return;
    }

    system_menu_redraw_ms_ += dt_ms;
    const uint32_t previous = system_menu_animation_ms_;
    if (system_menu_closing_) {
        system_menu_animation_ms_ = dt_ms >= system_menu_animation_ms_
            ? 0U : system_menu_animation_ms_ - dt_ms;
    } else {
        system_menu_animation_ms_ = std::min<uint32_t>(
            kSystemMenuDurationMs, system_menu_animation_ms_ + dt_ms);
    }

    if (system_menu_animation_ms_ != previous &&
        (system_menu_redraw_ms_ >= kSystemMenuFrameMs ||
         system_menu_animation_ms_ == 0U ||
         system_menu_animation_ms_ == kSystemMenuDurationMs)) {
        system_menu_redraw_ms_ %= kSystemMenuFrameMs;
        invalidate(systemMenuRect());
    }

    if (system_menu_closing_ && system_menu_animation_ms_ == 0U) {
        finishSystemMenuClose();
    }
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
    if (power_overlay_open_) return ESP_OK;
    if (!active_ || !redrawPending()) return ESP_OK;
    const gfx::Rect region = full_redraw_ ? gfx::Rect{0, 0, gfx_.width(), gfx_.height()} : dirty_;
    if (region.empty()) return ESP_OK;

    if (system_menu_open_ && system_menu_underlay_valid_) {
        gfx_.display().load_framebuffer(system_menu_underlay_.data(),
                                        system_menu_underlay_.size());
    }
    gfx_.set_clip(region);
    if (!system_menu_open_) active_->draw(context_, region);
    if (system_menu_open_) drawSystemMenu();
    gfx_.reset_clip();

    full_redraw_ = false;
    dirty_ = {};
    ++rendered_frames_;
    return gfx_.present();
}

gfx::Rect AppManager::systemMenuRect() const {
    return {kSystemMenuLeft, 0, kSystemMenuWidth, gfx_.height()};
}

int AppManager::systemMenuPanelX() const {
    const float progress = smooth_step(
        static_cast<float>(system_menu_animation_ms_) / kSystemMenuDurationMs);
    return interpolate(gfx_.width(), kSystemMenuLeft, progress);
}

int AppManager::systemMenuItemCount() const {
    const size_t quick_count = active_ ? std::min<size_t>(active_->quickActionCount(), 2U) : 0U;
    return static_cast<int>(quick_count) + 4;
}

void AppManager::openSystemMenu() {
    if (system_menu_open_) return;
    const uint8_t* framebuffer = gfx_.display().framebuffer();
    if (framebuffer) {
        std::memcpy(system_menu_underlay_.data(), framebuffer,
                    system_menu_underlay_.size());
        system_menu_underlay_valid_ = true;
    } else {
        system_menu_underlay_valid_ = false;
    }
    system_menu_open_ = true;
    system_menu_closing_ = false;
    system_menu_resume_on_close_ = true;
    system_menu_animation_ms_ = 0;
    system_menu_redraw_ms_ = kSystemMenuFrameMs;
    system_menu_launch_target_ = nullptr;
    system_menu_selected_ = 0;
    if (active_) active_->onSuspend(context_);
    haptics_.play(haptics::Effect::DoubleClick);
    if (settings_.uiSoundsEnabled()) audio_.play(audio::Effect::Click);
    invalidate(systemMenuRect());
}

void AppManager::startSystemMenuClose(bool resume_app, const char* launch_target) {
    if (!system_menu_open_ || system_menu_closing_) return;
    system_menu_closing_ = true;
    system_menu_resume_on_close_ = resume_app;
    system_menu_launch_target_ = launch_target;
    system_menu_redraw_ms_ = kSystemMenuFrameMs;
    invalidate(systemMenuRect());
}

void AppManager::finishSystemMenuClose() {
    if (!system_menu_open_) return;
    const char* launch_target = system_menu_launch_target_;
    const bool resume_app = system_menu_resume_on_close_;
    if (system_menu_underlay_valid_) {
        gfx_.display().load_framebuffer(system_menu_underlay_.data(),
                                        system_menu_underlay_.size());
    }
    system_menu_open_ = false;
    system_menu_closing_ = false;
    system_menu_underlay_valid_ = false;
    system_menu_launch_target_ = nullptr;
    if (launch_target) {
        launch(launch_target);
    } else {
        if (resume_app && active_) active_->onResume(context_);
        invalidate();
    }
}

void AppManager::closeSystemMenu(bool redraw_underlay, bool resume_app) {
    if (!system_menu_open_) return;
    if (system_menu_underlay_valid_) {
        gfx_.display().load_framebuffer(system_menu_underlay_.data(),
                                        system_menu_underlay_.size());
    }
    system_menu_open_ = false;
    system_menu_closing_ = false;
    system_menu_underlay_valid_ = false;
    system_menu_launch_target_ = nullptr;
    system_menu_animation_ms_ = 0;
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
    if (system_menu_closing_) return;
    const bool navigation = event.type == input::EventType::Pressed ||
                            event.type == input::EventType::Repeat;
    if (!navigation) return;

    const int quick_count = active_
        ? static_cast<int>(std::min<size_t>(active_->quickActionCount(), 2U)) : 0;
    const int item_count = systemMenuItemCount();
    const int volume_item = 1 + quick_count;
    const int settings_item = volume_item + 1;
    const int home_item = settings_item + 1;

    if (event.button == input::Button::Top) {
        system_menu_selected_ = (system_menu_selected_ + item_count - 1) % item_count;
        haptics_.play(haptics::Effect::Tick);
        if (settings_.uiSoundsEnabled()) audio_.play(audio::Effect::Tick);
        invalidate(systemMenuRect());
    } else if (event.button == input::Button::Down) {
        system_menu_selected_ = (system_menu_selected_ + 1) % item_count;
        haptics_.play(haptics::Effect::Tick);
        if (settings_.uiSoundsEnabled()) audio_.play(audio::Effect::Tick);
        invalidate(systemMenuRect());
    } else if (system_menu_selected_ == volume_item &&
               event.button == input::Button::Left) {
        adjustSystemVolume(-5);
    } else if (system_menu_selected_ == volume_item &&
               event.button == input::Button::Right) {
        adjustSystemVolume(5);
    } else if (event.type == input::EventType::Pressed &&
               (event.button == input::Button::B || event.button == input::Button::Menu)) {
        haptics_.play(haptics::Effect::Click);
        if (settings_.uiSoundsEnabled()) audio_.play(audio::Effect::Back);
        startSystemMenuClose(true);
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::A) {
        const int action = system_menu_selected_;
        if (action == volume_item) {
            adjustSystemVolume(5);
            return;
        }
        if (action > kResumeItem && action <= quick_count) {
            const bool ok = active_ && active_->runQuickAction(
                context_, static_cast<size_t>(action - 1));
            haptics_.play(ok ? haptics::Effect::Confirm : haptics::Effect::Alert);
            if (settings_.uiSoundsEnabled()) {
                audio_.play(ok ? audio::Effect::Confirm : audio::Effect::Error);
            }
            if (ok) startSystemMenuClose(true);
            return;
        }

        haptics_.play(haptics::Effect::Confirm);
        if (settings_.uiSoundsEnabled()) audio_.play(audio::Effect::Confirm);
        if (action == kResumeItem) {
            startSystemMenuClose(true);
        } else if (action == settings_item) {
            startSystemMenuClose(false, "settings");
        } else if (action == home_item) {
            startSystemMenuClose(false, home_ ? home_->id() : nullptr);
        }
    }
}

void AppManager::drawSystemMenu() {
    auto& canvas = gfx_.canvas();
    const int x = systemMenuPanelX();
    if (x >= gfx_.width()) return;

    canvas.fill_rect(x, 0, kSystemMenuWidth, gfx_.height(), gfx::WHITE);

    const int quick_count = active_
        ? static_cast<int>(std::min<size_t>(active_->quickActionCount(), 2U)) : 0;
    const int item_count = systemMenuItemCount();
    const int volume_item = 1 + quick_count;
    const int settings_item = volume_item + 1;
    const int home_item = settings_item + 1;

    for (int item = 0; item < item_count; ++item) {
        const int y = kSystemMenuRowY + item * kSystemMenuRowHeight;
        const bool selected = item == system_menu_selected_;
        if (selected) fill_pill(canvas, x + 5, y, kSystemMenuWidth - 10, 29, gfx::BLACK);

        const char* label = "";
        if (item == kResumeItem) {
            label = "resume";
        } else if (item <= quick_count) {
            label = active_->quickActionLabel(static_cast<size_t>(item - 1));
        } else if (item == volume_item) {
            label = "volume";
        } else if (item == settings_item) {
            label = "settings";
        } else if (item == home_item) {
            label = "home";
        }

        menu::PogoFont::drawText(
            canvas, x + 11, y - 1, label,
            selected ? menu::FontFace::Italic22 : menu::FontFace::Regular22,
            selected ? gfx::WHITE : gfx::BLACK);

        if (item == volume_item) {
            const int lit = std::clamp<int>((audio_.masterVolume() + 9) / 10, 0, 10);
            const gfx::Color bar_color = selected ? gfx::WHITE : gfx::BLACK;
            const int bars_x = x + kSystemMenuWidth - 49;
            for (int bar = 0; bar < 10; ++bar) {
                const int height = 3 + bar;
                if (bar < lit) {
                    canvas.fill_rect(bars_x + bar * 4, y + 22 - height, 3, height,
                                     bar_color);
                } else {
                    canvas.draw_vline(bars_x + bar * 4 + 1, y + 21, 1, bar_color);
                }
            }
        }
    }

    const int footer_y = 207;
    canvas.fill_rect(x + 4, footer_y, kSystemMenuWidth - 8, 31, gfx::WHITE);
    menu::PogoFont::drawText(canvas, x + 10, footer_y + 5,
                             active_ ? active_->title() : "pogopo",
                             menu::FontFace::Italic14);

    char battery[16]{};
    const power::State state = power_.state();
    if (state.battery_valid) {
        std::snprintf(battery, sizeof(battery), "%u%%",
                      static_cast<unsigned>(state.battery_percent));
    } else {
        std::snprintf(battery, sizeof(battery), "%s", state.usb_present ? "usb" : "--");
    }
    const int battery_width = menu::PogoFont::textWidth(menu::FontFace::Regular14, battery);
    menu::PogoFont::drawText(canvas, x + kSystemMenuWidth - 10 - battery_width,
                             footer_y + 5, battery, menu::FontFace::Regular14);
}

} // namespace pogopo::app
