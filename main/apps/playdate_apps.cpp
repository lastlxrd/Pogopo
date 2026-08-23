#include "apps/playdate_apps.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace pogopo::demo {
namespace {
constexpr char TAG[] = "pogodate_app";
constexpr uint32_t LCD_FRAME_MS = 20;

input::ButtonMask playdateButtons(input::ButtonMask raw) {
    constexpr input::ButtonMask supported =
        input::mask(input::Button::Top) |
        input::mask(input::Button::Down) |
        input::mask(input::Button::Left) |
        input::mask(input::Button::Right) |
        input::mask(input::Button::A) |
        input::mask(input::Button::B);
    input::ButtonMask translated = raw & supported;
    // Pogopo has an extra START key while Playdate games only know A/B.
    // Treat START as an A alias so title screens that say "Press A" also
    // behave naturally on Pogopo, without inventing a seventh Playdate bit.
    if (raw & input::mask(input::Button::Start)) {
        translated |= input::mask(input::Button::A);
    }
    return translated;
}

bool navigationEvent(const input::Event& event) {
    return event.type == input::EventType::Pressed ||
           event.type == input::EventType::Repeat;
}

bool endsPdx(const char* name) {
    if (!name) return false;
    const size_t length = std::strlen(name);
    if (length < 4U) return false;
    const char* suffix = name + length - 4U;
    return suffix[0] == '.' && (suffix[1] == 'p' || suffix[1] == 'P') &&
           (suffix[2] == 'd' || suffix[2] == 'D') &&
           (suffix[3] == 'x' || suffix[3] == 'X');
}
}

PogoDateApp::PogoDateApp(playdate::Game game, const char* app_id,
                         const char* app_title, const char* game_title)
    : game_(game), app_id_(app_id), app_title_(app_title),
      game_title_(game_title) {}

void PogoDateApp::preparePackage(const playdate::PackageInfo& package) {
    package_ = package;
}

const char* PogoDateApp::displayTitle() const {
    return package_.path[0] && package_.name[0] ? package_.name : game_title_;
}

void PogoDateApp::drawLoading(AppContext& context) {
    auto& canvas = context.gfx.canvas();
    context.gfx.reset_clip();
    canvas.clear(context.theme.background);
    gui::draw_header(canvas, context.theme, "POGODATE LITE", "LOADING");
    canvas.draw_rect(24, 49, 352, 139, context.theme.border);
    canvas.draw_text(43, 70, "PLAYDATE LUA COMPATIBILITY TEST",
                     gfx::font5x7(), context.theme.foreground);
    char game_line[80]{};
    std::snprintf(game_line, sizeof(game_line), "GAME: %s", displayTitle());
    canvas.draw_text(43, 99, game_line,
                     gfx::font5x7(), context.theme.foreground, 2);
    canvas.draw_text(43, 134, "LUA 5.4 + NATIVE POGOPO API",
                     gfx::font5x7(), context.theme.foreground);
    canvas.draw_text(43, 157, "400 x 240 / 1-BIT NATIVE",
                     gfx::font5x7(), context.theme.foreground);
    gui::draw_footer(canvas, context.theme, "PLEASE WAIT",
                     package_.path[0] ? "SD PDX / PDZ" : "SOURCE LUA");
    context.gfx.presentFull();
}

void PogoDateApp::onEnter(AppContext& context) {
    queued_pressed_ = 0;
    lcd_accumulator_ms_ = 0;
    last_perf_us_ = 0;
    previous_lua_frames_ = 0;
    lcd_frames_ = 0;
    previous_lcd_frames_ = 0;
    frame_pending_ = false;
    accelerometer_sequence_ = 0;
    accelerometer_x_ = 0.0f;
    accelerometer_y_ = 0.0f;
    accelerometer_z_ = 1.0f;
    accelerometer_initialized_ = false;

    context.audio.stopStream();
    context.audio.stopRealtime();
    drawLoading(context);
    start_error_ = package_.path[0]
        ? runtime_.startPackage(context.gfx.canvas(), context.audio,
                                context.storage, package_.path)
        : runtime_.start(context.gfx.canvas(), context.audio,
                         context.storage, game_);
    if (start_error_ == ESP_OK) {
        last_perf_us_ = esp_timer_get_time();
        context.haptics.play(haptics::Effect::Confirm);
        ESP_LOGI(TAG, "%s started from %s", displayTitle(),
                 package_.path[0] ? "SD main.pdz" : "embedded Playdate Lua source");
    } else {
        context.haptics.play(haptics::Effect::Alert);
        context.uiSound(audio::Effect::Error);
        ESP_LOGE(TAG, "PogoDate start failed: %s / %s",
                 esp_err_to_name(start_error_), runtime_.error());
    }
    context.invalidate();
}

void PogoDateApp::onExit(AppContext&) {
    runtime_.stop();
    queued_pressed_ = 0;
    frame_pending_ = false;
}

void PogoDateApp::onSuspend(AppContext&) {
    queued_pressed_ = 0;
    runtime_.setInput(0, 0);
}

void PogoDateApp::onResume(AppContext& context) {
    lcd_accumulator_ms_ = LCD_FRAME_MS;
    frame_pending_ = true;
    last_perf_us_ = esp_timer_get_time();
    const playdate::Stats stats = runtime_.stats();
    previous_lua_frames_ = stats.lua_frames;
    previous_lcd_frames_ = lcd_frames_;
    context.invalidate();
}

void PogoDateApp::onEvent(AppContext& context, const input::Event& event) {
    if (start_error_ != ESP_OK && package_.path[0] &&
        event.type == input::EventType::Pressed && event.button == input::Button::B) {
        context.launch("pogodate_browser");
        return;
    }
    if (event.type == input::EventType::Pressed) {
        queued_pressed_ = static_cast<input::ButtonMask>(
            queued_pressed_ | input::mask(event.button));
    }
}

void PogoDateApp::update(AppContext& context, uint32_t dt_ms) {
    if (start_error_ != ESP_OK || !runtime_.running()) return;

    const imu::Sample motion = context.imu.sample();
    if (motion.valid && motion.sequence != accelerometer_sequence_) {
        accelerometer_sequence_ = motion.sequence;

        // BMI270 is mounted with sensor +Y pointing toward the top of the
        // display. Playdate's public coordinate system uses +Y toward the
        // bottom, while +X and +Z already match the Pogopo board. A short
        // one-sample IIR removes 50 Hz sensor chatter without the sluggish
        // 170 ms visual filtering used by Motion Lab.
        const float mapped_x = std::clamp(motion.ax, -2.0f, 2.0f);
        const float mapped_y = std::clamp(-motion.ay, -2.0f, 2.0f);
        const float mapped_z = std::clamp(motion.az, -2.0f, 2.0f);
        constexpr float alpha = 0.60f;
        if (!accelerometer_initialized_) {
            accelerometer_x_ = mapped_x;
            accelerometer_y_ = mapped_y;
            accelerometer_z_ = mapped_z;
            accelerometer_initialized_ = true;
        } else {
            accelerometer_x_ += (mapped_x - accelerometer_x_) * alpha;
            accelerometer_y_ += (mapped_y - accelerometer_y_) * alpha;
            accelerometer_z_ += (mapped_z - accelerometer_z_) * alpha;
        }
    }
    runtime_.setAccelerometer(accelerometer_x_, accelerometer_y_,
                              accelerometer_z_, accelerometer_initialized_);
    runtime_.setInput(playdateButtons(context.input.heldMask()),
                      playdateButtons(queued_pressed_));
    const uint32_t produced = runtime_.update(dt_ms);
    if (produced > 0) {
        queued_pressed_ = 0;
        frame_pending_ = true;
    }

    if (!runtime_.running()) {
        start_error_ = ESP_FAIL;
        context.haptics.play(haptics::Effect::Alert);
        context.uiSound(audio::Effect::Error);
        context.invalidate();
        return;
    }

    lcd_accumulator_ms_ =
        std::min<uint32_t>(lcd_accumulator_ms_ + dt_ms, 100U);
    if (frame_pending_ && lcd_accumulator_ms_ >= LCD_FRAME_MS) {
        lcd_accumulator_ms_ -= LCD_FRAME_MS;
        frame_pending_ = false;
        ++lcd_frames_;
        context.invalidate();
    }

    const int64_t perf_now_us = esp_timer_get_time();
    const uint32_t perf_elapsed_us = static_cast<uint32_t>(
        std::max<int64_t>(1, perf_now_us - last_perf_us_));
    if (perf_elapsed_us >= 1000000U) {
        const playdate::Stats stats = runtime_.stats();
        const gfx::Graphics::Stats lcd_stats = context.gfx.stats();
        const uint32_t lua_delta =
            stats.lua_frames - previous_lua_frames_;
        const uint32_t lcd_delta =
            lcd_frames_ - previous_lcd_frames_;
        const uint32_t lua_fps = static_cast<uint32_t>(
            (static_cast<uint64_t>(lua_delta) * 1000000ULL +
             perf_elapsed_us / 2U) / perf_elapsed_us);
        const uint32_t lcd_fps = static_cast<uint32_t>(
            (static_cast<uint64_t>(lcd_delta) * 1000000ULL +
             perf_elapsed_us / 2U) / perf_elapsed_us);
        ESP_LOGI(
            TAG,
            "PERF PD %s lua=%lu lcd=%lu target=%lu update=%luus max=%luus "
            "logic=%luus blit=%luus rows=%u/240 tx=%luB/%luus "
            "luaheap=%lu peak=%lu gc=%lu err=%lu RAM=%lu/%lu PSRAM=%lu "
            "acc=%+.2f,%+.2f,%+.2f i2cerr=%lu",
            displayTitle(), static_cast<unsigned long>(lua_fps),
            static_cast<unsigned long>(lcd_fps),
            static_cast<unsigned long>(stats.requested_fps),
            static_cast<unsigned long>(stats.last_update_us),
            static_cast<unsigned long>(stats.max_update_us),
            static_cast<unsigned long>(stats.last_logic_us),
            static_cast<unsigned long>(stats.last_blit_us),
            static_cast<unsigned>(lcd_stats.last_rows),
            static_cast<unsigned long>(lcd_stats.last_bytes),
            static_cast<unsigned long>(lcd_stats.last_refresh_us),
            static_cast<unsigned long>(stats.lua_bytes),
            static_cast<unsigned long>(stats.lua_peak_bytes),
            static_cast<unsigned long>(stats.lua_gc_bytes),
            static_cast<unsigned long>(stats.errors),
            static_cast<unsigned long>(heap_caps_get_free_size(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned long>(heap_caps_get_largest_free_block(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned long>(heap_caps_get_free_size(
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
            static_cast<double>(accelerometer_x_),
            static_cast<double>(accelerometer_y_),
            static_cast<double>(accelerometer_z_),
            static_cast<unsigned long>(context.input.readErrors()));
        previous_lua_frames_ = stats.lua_frames;
        previous_lcd_frames_ = lcd_frames_;
        last_perf_us_ = perf_now_us;
    }
}

void PogoDateApp::draw(AppContext& context, const gfx::Rect&) {
    if (start_error_ == ESP_OK && runtime_.running()) {
        // The Lua graphics API draws into the native Sharp framebuffer during
        // update(). AppManager owns only the paced physical presentation.
        return;
    }

    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(context.theme.background);
    gui::draw_header(canvas, context.theme, "POGODATE LITE", "LUA ERROR");
    canvas.draw_rect(22, 47, 356, 150, context.theme.border);
    char failure[96]{};
    std::snprintf(failure, sizeof(failure), "%s COULD NOT CONTINUE",
                  displayTitle());
    canvas.draw_text(39, 67, failure,
                     gfx::font5x7(), context.theme.foreground, 2);
    char status[96]{};
    std::snprintf(status, sizeof(status), "%s", runtime_.error());
    canvas.draw_text(39, 111, status, gfx::font5x7(),
                     context.theme.foreground);
    canvas.draw_text(39, 151, "CHECK SERIAL FOR FULL DIAGNOSTIC",
                     gfx::font5x7(), context.theme.foreground);
    gui::draw_footer(canvas, context.theme, "POWER MENU -> HOME",
                     esp_err_to_name(start_error_));
}

void PogoDateBrowserApp::rescan(AppContext& context) {
    package_count_ = 0;
    for (auto& package : packages_) package = {};
    for (auto& item : items_) item = {};
    for (auto& subtitle : subtitles_) subtitle.fill('\0');

    if (!context.storage.mounted()) {
        ESP_LOGI(TAG, "Playdate browser requested SD remount");
        const esp_err_t mount_error = context.storage.remount();
        if (mount_error != ESP_OK) {
            items_[0] = {"SD CARD NOT READY", "Insert card, then START", nullptr, false};
            list_.setItems(items_.data(), 1);
            std::snprintf(status_, sizeof(status_), "MOUNT FAILED: %s",
                          esp_err_to_name(mount_error));
            ESP_LOGW(TAG, "Playdate SD remount failed: %s",
                     esp_err_to_name(mount_error));
            return;
        }
    }
    if (!context.storage.mounted()) {
        items_[0] = {"SD CARD NOT READY", "Mount failed", nullptr, false};
        list_.setItems(items_.data(), 1);
        std::snprintf(status_, sizeof(status_), "SD IS NOT MOUNTED");
        return;
    }
    char folder[192]{};
    std::snprintf(folder, sizeof(folder), "%s/playdate", context.storage.mountPoint());
    mkdir(folder, 0775);
    DIR* directory = opendir(folder);
    if (directory) {
        while (package_count_ < packages_.size()) {
            dirent* entry = readdir(directory);
            if (!entry) break;
            if (!endsPdx(entry->d_name)) continue;
            char path[224]{};
            if (std::snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name) >=
                static_cast<int>(sizeof(path))) continue;
            struct stat value{};
            if (stat(path, &value) != 0 || !S_ISDIR(value.st_mode)) continue;
            playdate::PackageInfo info{};
            const esp_err_t inspect_error = playdate::inspectPackage(path, info);
            if (inspect_error != ESP_OK) {
                ESP_LOGW(TAG, "Skipping PDX %s: %s", path,
                         esp_err_to_name(inspect_error));
                continue;
            }
            packages_[package_count_] = info;
            std::snprintf(subtitles_[package_count_].data(), subtitles_[package_count_].size(),
                          "%s  %u LUA MODULES",
                          playdate::packageKindName(info.kind),
                          static_cast<unsigned>(info.lua_modules));
            items_[package_count_] = {packages_[package_count_].name,
                                      subtitles_[package_count_].data(),
                                      packages_[package_count_].path, true};
            ESP_LOGI(TAG, "Found PDX: %s (%s, %u Lua modules)",
                     packages_[package_count_].name,
                     playdate::packageKindName(info.kind),
                     static_cast<unsigned>(info.lua_modules));
            ++package_count_;
        }
        closedir(directory);
    }
    if (package_count_ == 0) {
        items_[0] = {"NO .PDX FOLDERS FOUND", "Put them in /playdate", nullptr, false};
        list_.setItems(items_.data(), 1);
        std::snprintf(status_, sizeof(status_), "EXTRACT GAME.PDX TO /playdate");
        return;
    }
    list_.setItems(items_.data(), package_count_);
    list_.setSelected(0);
    std::snprintf(status_, sizeof(status_), "%u PACKAGES  START RESCANS",
                  static_cast<unsigned>(package_count_));
}

void PogoDateBrowserApp::onEnter(AppContext& context) {
    list_.setRowHeight(32);
    rescan(context);
    context.invalidate();
}

void PogoDateBrowserApp::launchSelected(AppContext& context) {
    if (package_count_ == 0 || list_.selected() >= package_count_) return;
    const playdate::PackageInfo& package = packages_[list_.selected()];
    if (package.kind == playdate::PackageKind::NativeBinary) {
        std::snprintf(status_, sizeof(status_), "ARM PDEX.BIN: NOT EXECUTABLE ON XTENSA");
        context.haptics.play(haptics::Effect::Alert);
        context.uiSound(audio::Effect::Error);
        context.invalidate();
        return;
    }
    player_.preparePackage(package);
    context.haptics.play(haptics::Effect::Confirm);
    context.uiSound(audio::Effect::Confirm);
    context.launch("pogodate_player");
}

void PogoDateBrowserApp::onEvent(AppContext& context, const input::Event& event) {
    if (!navigationEvent(event)) return;
    if (event.button == input::Button::Top && list_.move(-1)) {
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate(list_.bounds());
    } else if (event.button == input::Button::Down && list_.move(1)) {
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate(list_.bounds());
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::A) {
        launchSelected(context);
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::Start) {
        rescan(context);
        context.haptics.play(haptics::Effect::Click);
        context.invalidate();
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Back);
        context.home();
    }
}

void PogoDateBrowserApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(context.theme.background);
    gui::draw_header(canvas, context.theme, "PLAYDATE SD", "PDX / PDZ");
    list_.draw(canvas, context.theme);
    canvas.draw_text(22, 198, status_, gfx::font5x7(), context.theme.foreground);
    gui::draw_footer(canvas, context.theme, "A PLAY  B BACK", "START RESCAN");
}

} // namespace pogopo::demo
