#include "apps/gameboy_apps.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "esp_log.h"

namespace pogopo::demo {
namespace {
constexpr char TAG[] = "gb_apps";
constexpr uint32_t EXIT_HOLD_MS = 650;

bool navigation_event(const input::Event& event) {
    return event.type == input::EventType::Pressed ||
           event.type == input::EventType::Repeat;
}

void copy_text(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0) return;
    if (!source) {
        destination[0] = '\0';
        return;
    }
    const size_t count = std::min(capacity - 1U, std::strlen(source));
    std::memcpy(destination, source, count);
    destination[count] = '\0';
}

} // namespace

void GameBoyApp::prepare(const char* path, const char* display_name,
                         gameboy::ScaleMode scale) {
    copy_text(rom_path_, sizeof(rom_path_), path);
    copy_text(display_name_, sizeof(display_name_), display_name);
    scale_ = scale;
}

void GameBoyApp::drawLoading(AppContext& context) {
    auto& canvas = context.gfx.canvas();
    context.gfx.reset_clip();
    canvas.clear(context.theme.background);
    gui::draw_header(canvas, context.theme, "GAME BOY", "LOADING");
    canvas.draw_rect(28, 54, 344, 118, context.theme.border);
    canvas.draw_text(48, 76, "PEANUT-GB / MINIGB APU", gfx::font5x7(),
                     context.theme.foreground);
    canvas.draw_text(48, 99, display_name_[0] ? display_name_ : "ROM",
                     gfx::font5x7(), context.theme.foreground);
    canvas.draw_text(48, 130, "READING ROM + SAVE RAM...", gfx::font5x7(),
                     context.theme.foreground);
    gui::draw_footer(canvas, context.theme, "PLEASE WAIT", gameboy::scale_mode_name(scale_));
    context.gfx.presentFull();
}

void GameBoyApp::onEnter(AppContext& context) {
    exit_hold_ms_ = 0;
    last_sequence_ = UINT32_MAX;
    force_draw_ = true;
    load_error_ = ESP_ERR_INVALID_ARG;

    context.audio.stopStream();
    drawLoading(context);

    if (rom_path_[0]) {
        load_error_ = emulator_.load(rom_path_);
    }
    if (load_error_ == ESP_OK) {
        context.haptics.play(haptics::Effect::Confirm);
        ESP_LOGI(TAG, "Game started: %s", emulator_.romTitle());
    } else {
        context.haptics.play(haptics::Effect::Alert);
        context.uiSound(audio::Effect::Error);
        ESP_LOGE(TAG, "Game load failed: %s (%s)", rom_path_, esp_err_to_name(load_error_));
    }
    context.invalidate();
}

void GameBoyApp::onExit(AppContext&) {
    emulator_.unload();
    exit_hold_ms_ = 0;
}

void GameBoyApp::onEvent(AppContext& context, const input::Event& event) {
    if (load_error_ != ESP_OK && event.type == input::EventType::Pressed &&
        event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Back);
        context.launch("gb_browser");
    }
}

void GameBoyApp::updateButtons(AppContext& context) {
    const input::ButtonMask held = context.input.heldMask();
    gameboy::Buttons buttons;
    buttons.up = (held & input::mask(input::Button::Top)) != 0;
    buttons.down = (held & input::mask(input::Button::Down)) != 0;
    buttons.left = (held & input::mask(input::Button::Left)) != 0;
    buttons.right = (held & input::mask(input::Button::Right)) != 0;
    buttons.a = (held & input::mask(input::Button::A)) != 0;
    buttons.b = (held & input::mask(input::Button::B)) != 0;
    buttons.start = (held & input::mask(input::Button::Start)) != 0;
    buttons.select = (held & input::mask(input::Button::Menu)) != 0;
    emulator_.setButtons(buttons);
}

void GameBoyApp::update(AppContext& context, uint32_t dt_ms) {
    if (load_error_ != ESP_OK || !emulator_.loaded()) return;
    updateButtons(context);

    const input::ButtonMask exit_combo =
        input::mask(input::Button::B) | input::mask(input::Button::Start);
    if (context.input.comboHeld(exit_combo)) {
        exit_hold_ms_ = std::min<uint32_t>(EXIT_HOLD_MS, exit_hold_ms_ + dt_ms);
        if (exit_hold_ms_ >= EXIT_HOLD_MS) {
            context.haptics.play(haptics::Effect::Heavy);
            context.uiSound(audio::Effect::Back);
            context.launch("gb_browser");
            return;
        }
    } else {
        exit_hold_ms_ = 0;
    }

    const uint32_t sequence = emulator_.frameSequence();
    if (sequence != last_sequence_) {
        last_sequence_ = sequence;
        context.invalidate({0, 0, context.gfx.width(), context.gfx.height()});
    }
}

void GameBoyApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    if (load_error_ != ESP_OK) {
        canvas.clear_clip(context.theme.background);
        gui::draw_header(canvas, context.theme, "GAME BOY", "LOAD ERROR");
        canvas.draw_rect(25, 48, 350, 145, context.theme.border);
        canvas.draw_text(43, 69, "COULD NOT START ROM", gfx::font5x7(),
                         context.theme.foreground, 2);
        canvas.draw_text(43, 107, display_name_, gfx::font5x7(),
                         context.theme.foreground);
        canvas.draw_text(43, 132, esp_err_to_name(load_error_), gfx::font5x7(),
                         context.theme.foreground);
        canvas.draw_text(43, 157, "ONLY UNCOMPRESSED .GB IS SUPPORTED", gfx::font5x7(),
                         context.theme.foreground);
        gui::draw_footer(canvas, context.theme, "B BACK", "CHECK SERIAL");
        return;
    }

    emulator_.drawLatest(canvas, scale_, force_draw_);
    force_draw_ = false;
}

void GameBoyBrowserApp::rescan(AppContext& context) {
    file_count_ = 0;
    for (auto& file : files_) file = {};
    for (auto& item : items_) item = {};
    for (auto& subtitle : subtitles_) subtitle.fill('\0');

    if (!context.storage.mounted()) {
        items_[0] = {"SD CARD NOT READY", "Mount failed", nullptr, false};
        list_.setItems(items_.data(), 1);
        copy_text(status_, sizeof(status_), "SD IS NOT MOUNTED");
        return;
    }

    char folder[192]{};
    const char* mount = context.storage.mountPoint();
    const size_t mount_length = std::strlen(mount);
    constexpr char suffix[] = "/gameboy";
    if (mount_length + sizeof(suffix) <= sizeof(folder)) {
        std::memcpy(folder, mount, mount_length);
        std::memcpy(folder + mount_length, suffix, sizeof(suffix));
        mkdir(folder, 0775);
    }

    file_count_ = context.storage.listFiles(
        "/gameboy", ".gb", files_.data(), files_.size());
    if (file_count_ == 0) {
        items_[0] = {"NO .GB ROMS FOUND", "Put files in /gameboy", nullptr, false};
        list_.setItems(items_.data(), 1);
        copy_text(status_, sizeof(status_), "FOLDER: /gameboy");
        return;
    }

    for (size_t i = 0; i < file_count_; ++i) {
        const uint32_t kib = files_[i].size / 1024U;
        std::snprintf(subtitles_[i].data(), subtitles_[i].size(),
                      "%lu KiB  DMG", static_cast<unsigned long>(kib));
        items_[i] = {files_[i].name, subtitles_[i].data(), files_[i].path, true};
    }
    list_.setItems(items_.data(), file_count_);
    list_.setSelected(0);

    std::snprintf(status_, sizeof(status_), "%u ROMS  %s",
                  static_cast<unsigned>(file_count_), gameboy::scale_mode_name(scale_));
}

void GameBoyBrowserApp::onEnter(AppContext& context) {
    list_.setRowHeight(29);
    rescan(context);
    context.invalidate();
}

void GameBoyBrowserApp::launchSelected(AppContext& context) {
    if (file_count_ == 0 || list_.selected() >= file_count_) {
        context.haptics.play(haptics::Effect::Alert);
        context.uiSound(audio::Effect::Error);
        return;
    }
    const size_t selected = list_.selected();
    player_.prepare(files_[selected].path, files_[selected].name, scale_);
    context.haptics.play(haptics::Effect::Confirm);
    context.uiSound(audio::Effect::Confirm);
    context.launch("gameboy");
}

void GameBoyBrowserApp::onEvent(AppContext& context, const input::Event& event) {
    if (!navigation_event(event)) return;

    if (event.button == input::Button::Top && list_.move(-1)) {
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate(list_.bounds());
    } else if (event.button == input::Button::Down && list_.move(1)) {
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate(list_.bounds());
    } else if (event.type == input::EventType::Pressed &&
               (event.button == input::Button::Left || event.button == input::Button::Right)) {
        scale_ = scale_ == gameboy::ScaleMode::FitHeight
            ? gameboy::ScaleMode::OneX
            : gameboy::ScaleMode::FitHeight;
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        std::snprintf(status_, sizeof(status_), "%u ROMS  %s",
                      static_cast<unsigned>(file_count_), gameboy::scale_mode_name(scale_));
        context.invalidate();
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::A) {
        launchSelected(context);
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::Start) {
        rescan(context);
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Click);
        context.invalidate();
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Back);
        context.home();
    }
}

void GameBoyBrowserApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(context.theme.background);
    gui::draw_header(canvas, context.theme, "GAME BOY", gameboy::scale_mode_name(scale_));
    list_.draw(canvas, context.theme);
    canvas.draw_text(22, 198, status_, gfx::font5x7(), context.theme.foreground);
    gui::draw_footer(canvas, context.theme, "A PLAY  B BACK", "L/R SCALE  START SCAN");
}

} // namespace pogopo::demo
