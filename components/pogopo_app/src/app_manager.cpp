#include "pogopo/app/app_manager.h"

#include <algorithm>
#include <cstring>

namespace pogopo::app {

namespace {
constexpr gui::ListItem kSystemMenuItems[] = {
    {"Resume", "Return to app", "resume", true},
    {"Home", "Open launcher", "home", true},
    {"About", "System information", "about", true},
};
}

void Context::invalidate() { manager.invalidate(); }
void Context::invalidate(const gfx::Rect& region) { manager.invalidate(region); }
bool Context::launch(const char* id) { return manager.launch(id); }
bool Context::home() { return manager.home(); }

AppManager::AppManager(gfx::Graphics& graphics, input::Input& input, haptics::Haptics& haptics)
    : gfx_(graphics), input_(input), haptics_(haptics),
      context_(graphics, input, haptics, theme_, *this) {}

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
    closeSystemMenu(false);
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

void AppManager::processInput() {
    input::Event event;
    while (input_.nextEvent(event, 0)) {
        if (system_menu_open_) {
            handleSystemMenu(event);
            continue;
        }
        if (event.type == input::EventType::Pressed && event.button == input::Button::Menu) {
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
    return {84, 43, 232, 154};
}

void AppManager::openSystemMenu() {
    system_menu_open_ = true;
    system_menu_selected_ = 0;
    haptics_.play(haptics::Effect::DoubleClick);
    invalidate(systemMenuRect());
}

void AppManager::closeSystemMenu(bool redraw_underlay) {
    if (!system_menu_open_) return;
    system_menu_open_ = false;
    if (redraw_underlay) invalidate();
}

void AppManager::handleSystemMenu(const input::Event& event) {
    const bool navigation = event.type == input::EventType::Pressed ||
                            event.type == input::EventType::Repeat;
    if (!navigation) return;

    if (event.button == input::Button::Top) {
        system_menu_selected_ = (system_menu_selected_ + 2) % 3;
        haptics_.play(haptics::Effect::Tick);
        invalidate(systemMenuRect());
    } else if (event.button == input::Button::Down) {
        system_menu_selected_ = (system_menu_selected_ + 1) % 3;
        haptics_.play(haptics::Effect::Tick);
        invalidate(systemMenuRect());
    } else if (event.type == input::EventType::Pressed &&
               (event.button == input::Button::B || event.button == input::Button::Menu)) {
        haptics_.play(haptics::Effect::Click);
        closeSystemMenu(true);
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::A) {
        haptics_.play(haptics::Effect::Confirm);
        const int action = system_menu_selected_;
        closeSystemMenu(false);
        if (action == 0) invalidate();
        else if (action == 1) home();
        else if (action == 2) launch("about");
    }
}

void AppManager::drawSystemMenu() {
    gui::Dialog dialog(systemMenuRect());
    dialog.setTitle("SYSTEM MENU");
    dialog.setMessage("Use UP/DOWN and A. B or MENU closes this overlay.");
    dialog.setFooter("A SELECT   B BACK");
    dialog.draw(gfx_.canvas(), theme_);

    gui::List list({systemMenuRect().x + 18, systemMenuRect().y + 62,
                    systemMenuRect().w - 36, 67});
    list.setItems(kSystemMenuItems, 3);
    list.setRowHeight(21);
    list.setSelected(static_cast<size_t>(system_menu_selected_));
    list.draw(gfx_.canvas(), theme_);
}

} // namespace pogopo::app
