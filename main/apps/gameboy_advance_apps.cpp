#include "apps/gameboy_advance_apps.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace pogopo::demo {
namespace {

constexpr char TAG[] = "gba_apps";
constexpr uint32_t EXIT_HOLD_MS = 650;

bool navigation_event(const input::Event& event) {
    return event.type == input::EventType::Pressed ||
           event.type == input::EventType::Repeat;
}

void copy_text(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0) return;
    destination[0] = '\0';
    if (!source) return;
    const size_t count = std::min(capacity - 1U, std::strlen(source));
    std::memcpy(destination, source, count);
    destination[count] = '\0';
}

} // namespace

void GameBoyAdvanceApp::prepare(const char* path, const char* display_name,
                                gameboy_advance::ScaleMode scale) {
    copy_text(rom_path_, sizeof(rom_path_), path);
    copy_text(display_name_, sizeof(display_name_), display_name);
    scale_ = scale;
}

void GameBoyAdvanceApp::drawLoading(AppContext& context) {
    auto& canvas = context.gfx.canvas();
    context.gfx.reset_clip();
    canvas.clear(context.theme.background);
    gui::draw_header(canvas, context.theme, "GAME BOY ADVANCE", "EXPERIMENTAL");
    canvas.draw_rect(28, 48, 344, 130, context.theme.border);
    canvas.draw_text(48, 68, "GPSP / XTENSA INTERPRETER", gfx::font5x7(),
                     context.theme.foreground);
    canvas.draw_text(48, 92, display_name_[0] ? display_name_ : "ROM",
                     gfx::font5x7(), context.theme.foreground);
    canvas.draw_text(48, 119, "ALLOCATING PSRAM CACHE...", gfx::font5x7(),
                     context.theme.foreground);
    canvas.draw_text(48, 143, "FIRST LOAD CAN TAKE A MOMENT", gfx::font5x7(),
                     context.theme.foreground);
    gui::draw_footer(canvas, context.theme, "PLEASE WAIT",
                     gameboy_advance::scale_mode_name(scale_));
    context.gfx.presentFull();
}

void GameBoyAdvanceApp::onEnter(AppContext& context) {
    exit_hold_ms_ = 0;
    last_sequence_ = UINT32_MAX;
    force_draw_ = true;
    perf_elapsed_ms_ = 0;
    perf_previous_ = {};
    load_error_ = ESP_ERR_INVALID_ARG;
    context.audio.stopStream();
    drawLoading(context);

    if (rom_path_[0]) load_error_ = emulator_.load(rom_path_);
    if (load_error_ == ESP_OK) {
        context.haptics.play(haptics::Effect::Confirm);
        ESP_LOGI(TAG, "GBA game started: %s", emulator_.romTitle());
    } else {
        context.haptics.play(haptics::Effect::Alert);
        context.uiSound(audio::Effect::Error);
        ESP_LOGE(TAG, "GBA load failed: %s (%s)", rom_path_,
                 esp_err_to_name(load_error_));
    }
    context.invalidate();
}

void GameBoyAdvanceApp::onExit(AppContext&) {
    emulator_.unload();
    exit_hold_ms_ = 0;
}

void GameBoyAdvanceApp::onSuspend(AppContext&) {
    emulator_.setButtons({});
    emulator_.setPaused(true);
    exit_hold_ms_ = 0;
}

void GameBoyAdvanceApp::onResume(AppContext&) {
    emulator_.setPaused(false);
    force_draw_ = true;
}

void GameBoyAdvanceApp::onEvent(AppContext& context, const input::Event& event) {
    if (load_error_ != ESP_OK && event.type == input::EventType::Pressed &&
        event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Back);
        context.launch("gba_browser");
    }
}

void GameBoyAdvanceApp::updateButtons(AppContext& context) {
    const input::ButtonMask held = context.input.heldMask();
    const bool menu = (held & input::mask(input::Button::Menu)) != 0;
    const bool a = (held & input::mask(input::Button::A)) != 0;
    const bool b = (held & input::mask(input::Button::B)) != 0;

    gameboy_advance::Buttons buttons;
    buttons.up = (held & input::mask(input::Button::Top)) != 0;
    buttons.down = (held & input::mask(input::Button::Down)) != 0;
    buttons.left = (held & input::mask(input::Button::Left)) != 0;
    buttons.right = (held & input::mask(input::Button::Right)) != 0;
    buttons.start = (held & input::mask(input::Button::Start)) != 0;
    buttons.select = menu && !a && !b;
    buttons.l = menu && a;
    buttons.r = menu && b;
    buttons.a = a && !menu;
    buttons.b = b && !menu;
    emulator_.setButtons(buttons);
}

void GameBoyAdvanceApp::update(AppContext& context, uint32_t dt_ms) {
    if (load_error_ != ESP_OK || !emulator_.loaded()) return;
    updateButtons(context);

    const input::ButtonMask exit_combo =
        input::mask(input::Button::B) | input::mask(input::Button::Start);
    if (context.input.comboHeld(exit_combo)) {
        exit_hold_ms_ = std::min<uint32_t>(EXIT_HOLD_MS, exit_hold_ms_ + dt_ms);
        if (exit_hold_ms_ >= EXIT_HOLD_MS) {
            context.haptics.play(haptics::Effect::Heavy);
            context.uiSound(audio::Effect::Back);
            context.launch("gba_browser");
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

    perf_elapsed_ms_ += dt_ms;
    if (perf_elapsed_ms_ >= 1000U) {
        const gameboy_advance::Stats now = emulator_.stats();
        const uint32_t emu = now.emulated_frames - perf_previous_.emulated_frames;
        const uint32_t render = now.rendered_frames - perf_previous_.rendered_frames;
        const uint32_t lcd = now.displayed_frames - perf_previous_.displayed_frames;
        const uint32_t loads = now.page_loads - perf_previous_.page_loads;
        const uint32_t load_us = now.page_load_us - perf_previous_.page_load_us;
        const uint32_t drop = now.audio_frames_dropped -
                              perf_previous_.audio_frames_dropped;
        const audio::RealtimeInfo realtime = context.audio.realtimeInfo();
        ESP_LOGI(TAG,
                 "PERF GBA emu=%lu render=%lu lcd=%lu core=%luus max=%luus "
                 "ROM=%luKiB cache=%luKiB swap=%s page=%lu/%luus "
                 "code=OFF IWRAM=%s rate=%luHz audio=%lu/%lu under=%lu "
                 "over=%lu drop=%lu RAM=%lu/%lu PSRAM=%lu i2cerr=%lu",
                 static_cast<unsigned long>(emu),
                 static_cast<unsigned long>(render),
                 static_cast<unsigned long>(lcd),
                 static_cast<unsigned long>(now.last_frame_us),
                 static_cast<unsigned long>(now.max_frame_us),
                 static_cast<unsigned long>(now.rom_bytes / 1024U),
                 static_cast<unsigned long>(now.rom_buffer_bytes / 1024U),
                 now.rom_bytes > now.rom_buffer_bytes ? "SD" : "FULL",
                 static_cast<unsigned long>(loads),
                 static_cast<unsigned long>(load_us),
                 now.iwram_internal ? "INT" : "PSRAM",
                 static_cast<unsigned long>(now.audio_source_rate),
                 static_cast<unsigned long>(realtime.buffered_frames),
                 static_cast<unsigned long>(realtime.capacity_frames),
                 static_cast<unsigned long>(realtime.underruns),
                 static_cast<unsigned long>(realtime.overruns),
                 static_cast<unsigned long>(drop),
                 static_cast<unsigned long>(heap_caps_get_free_size(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned long>(heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned long>(heap_caps_get_free_size(
                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                 static_cast<unsigned long>(context.input.readErrors()));
        perf_previous_ = now;
        perf_elapsed_ms_ = 0;
    }
}

void GameBoyAdvanceApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    if (load_error_ != ESP_OK) {
        canvas.clear_clip(context.theme.background);
        gui::draw_header(canvas, context.theme, "GAME BOY ADVANCE", "LOAD ERROR");
        canvas.draw_rect(25, 48, 350, 145, context.theme.border);
        canvas.draw_text(43, 68, "COULD NOT START GBA ROM", gfx::font5x7(),
                         context.theme.foreground, 2);
        canvas.draw_text(43, 108, display_name_, gfx::font5x7(),
                         context.theme.foreground);
        canvas.draw_text(43, 133, esp_err_to_name(load_error_), gfx::font5x7(),
                         context.theme.foreground);
        canvas.draw_text(43, 158, "UNCOMPRESSED .GBA / MAX 32 MiB", gfx::font5x7(),
                         context.theme.foreground);
        gui::draw_footer(canvas, context.theme, "B BACK", "CHECK SERIAL");
        return;
    }
    emulator_.drawLatest(canvas, scale_, force_draw_);
    force_draw_ = false;
}

void GameBoyAdvanceBrowserApp::rescan(AppContext& context) {
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
    constexpr char suffix[] = "/gameboyadvance";
    if (mount_length + sizeof(suffix) <= sizeof(folder)) {
        std::memcpy(folder, mount, mount_length);
        std::memcpy(folder + mount_length, suffix, sizeof(suffix));
        mkdir(folder, 0775);
    }

    file_count_ = context.storage.listFiles(
        "/gameboyadvance", ".gba", files_.data(), files_.size());
    if (file_count_ == 0) {
        items_[0] = {"NO .GBA ROMS FOUND", "Put files in /gameboyadvance", nullptr, false};
        list_.setItems(items_.data(), 1);
        copy_text(status_, sizeof(status_), "FOLDER: /gameboyadvance");
        return;
    }

    for (size_t i = 0; i < file_count_; ++i) {
        const uint32_t mib = (files_[i].size + 512U * 1024U) / (1024U * 1024U);
        std::snprintf(subtitles_[i].data(), subtitles_[i].size(),
                      "%lu MiB  GBA", static_cast<unsigned long>(mib));
        items_[i] = {files_[i].name, subtitles_[i].data(), files_[i].path, true};
    }
    list_.setItems(items_.data(), file_count_);
    list_.setSelected(0);
    std::snprintf(status_, sizeof(status_), "%u ROMS  %s",
                  static_cast<unsigned>(file_count_),
                  gameboy_advance::scale_mode_name(scale_));
}

void GameBoyAdvanceBrowserApp::onEnter(AppContext& context) {
    list_.setRowHeight(29);
    rescan(context);
    context.invalidate();
}

void GameBoyAdvanceBrowserApp::launchSelected(AppContext& context) {
    if (file_count_ == 0 || list_.selected() >= file_count_) {
        context.haptics.play(haptics::Effect::Alert);
        context.uiSound(audio::Effect::Error);
        return;
    }
    const size_t selected = list_.selected();
    player_.prepare(files_[selected].path, files_[selected].name, scale_);
    context.haptics.play(haptics::Effect::Confirm);
    context.uiSound(audio::Effect::Confirm);
    context.launch("gameboy_advance");
}

void GameBoyAdvanceBrowserApp::onEvent(AppContext& context,
                                       const input::Event& event) {
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
               (event.button == input::Button::Left ||
                event.button == input::Button::Right)) {
        scale_ = scale_ == gameboy_advance::ScaleMode::FitHeight
            ? gameboy_advance::ScaleMode::OneX
            : gameboy_advance::ScaleMode::FitHeight;
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        std::snprintf(status_, sizeof(status_), "%u ROMS  %s",
                      static_cast<unsigned>(file_count_),
                      gameboy_advance::scale_mode_name(scale_));
        context.invalidate();
    } else if (event.type == input::EventType::Pressed &&
               event.button == input::Button::A) {
        launchSelected(context);
    } else if (event.type == input::EventType::Pressed &&
               event.button == input::Button::Start) {
        rescan(context);
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Click);
        context.invalidate();
    } else if (event.type == input::EventType::Pressed &&
               event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Back);
        context.home();
    }
}

void GameBoyAdvanceBrowserApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(context.theme.background);
    gui::draw_header(canvas, context.theme, "GAME BOY ADVANCE", "GPSP TEST");
    list_.draw(canvas, context.theme);
    canvas.draw_text(22, 198, status_, gfx::font5x7(), context.theme.foreground);
    gui::draw_footer(canvas, context.theme, "A PLAY  B BACK", "L/R SCALE  START SCAN");
}

} // namespace pogopo::demo
