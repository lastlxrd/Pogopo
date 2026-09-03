#include "apps/playdate_apps.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pogopo_gameboy.h"
#include "pogopo_menu.h"

namespace pogopo::demo {
namespace {
constexpr char TAG[] = "pogodate_app";
constexpr uint32_t LCD_FRAME_MS = 20;

constexpr input::ButtonMask playdateButtons(input::ButtonMask raw,
                                            bool swap_horizontal,
                                            bool start_alias) {
    constexpr input::ButtonMask supported =
        input::mask(input::Button::Top) |
        input::mask(input::Button::Down) |
        input::mask(input::Button::Left) |
        input::mask(input::Button::Right) |
        input::mask(input::Button::A) |
        input::mask(input::Button::B);
    input::ButtonMask translated = raw & supported;
    // Maze 1.1.0's horizontal menu handlers are reversed on Pogopo even
    // though the shared physical mapping is correct for the other PDX games.
    // Keep this compatibility quirk package-scoped: the STEP11.6.7 global
    // swap mirrored every game's controls.
    if (swap_horizontal) {
        constexpr input::ButtonMask left = input::mask(input::Button::Left);
        constexpr input::ButtonMask right = input::mask(input::Button::Right);
        translated = static_cast<input::ButtonMask>(
            translated & ~(left | right));
        if (raw & left) {
            translated = static_cast<input::ButtonMask>(translated | right);
        }
        if (raw & right) {
            translated = static_cast<input::ButtonMask>(translated | left);
        }
    }
    // Pogopo has an extra START key while Playdate games only know A/B.
    // Treat START as an A alias so title screens that say "Press A" also
    // behave naturally on Pogopo, without inventing a seventh Playdate bit.
    if (start_alias && (raw & input::mask(input::Button::Start))) {
        translated |= input::mask(input::Button::A);
    }
    return translated;
}

static_assert(playdateButtons(input::mask(input::Button::Left), false, true) ==
              input::mask(input::Button::Left));
static_assert(playdateButtons(input::mask(input::Button::Right), false, true) ==
              input::mask(input::Button::Right));
static_assert(playdateButtons(input::mask(input::Button::Left), true, true) ==
              input::mask(input::Button::Right));
static_assert(playdateButtons(input::mask(input::Button::Right), true, true) ==
              input::mask(input::Button::Left));
static_assert(playdateButtons(input::mask(input::Button::Start), false, true) ==
              input::mask(input::Button::A));
static_assert(playdateButtons(input::mask(input::Button::Start), false, false) ==
              0);

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
                         const char* app_title, const char* game_title,
                         gameboy::GameBoy* memory_donor)
    : game_(game), app_id_(app_id), app_title_(app_title),
      game_title_(game_title), memory_donor_(memory_donor) {}

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
    menu::PogoFont::drawText(canvas, 43, 64,
                             "Playdate Lua compatibility",
                             menu::FontFace::Regular14,
                             context.theme.foreground);
    char game_line[80]{};
    std::snprintf(game_line, sizeof(game_line), "GAME: %s", displayTitle());
    menu::PogoFont::drawText(canvas, 43, 91, game_line,
                             menu::FontFace::Italic14,
                             context.theme.foreground);
    menu::PogoFont::drawText(canvas, 43, 126,
                             "Lua 5.4 + native Pogopo API",
                             menu::FontFace::Regular14,
                             context.theme.foreground);
    menu::PogoFont::drawText(canvas, 43, 151,
                             "400 x 240 / 1-bit native",
                             menu::FontFace::Regular14,
                             context.theme.foreground);
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
    borrowed_rom_arena_bytes_ = memory_donor_
        ? memory_donor_->releaseIdleRomArena() : 0;
    ESP_LOGI(TAG, "PogoDate fast-RAM handoff: borrowed=%luB free=%luB largest=%luB",
             static_cast<unsigned long>(borrowed_rom_arena_bytes_),
             static_cast<unsigned long>(heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned long>(heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    ESP_LOGI(TAG, "Starting %s: pogopo_os stack free=%uB",
             displayTitle(), static_cast<unsigned>(
                 uxTaskGetStackHighWaterMark(nullptr)));
    start_error_ = package_.path[0]
        ? runtime_.startPackage(context.gfx.canvas(), context.audio,
                                context.storage, package_)
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
        if (borrowed_rom_arena_bytes_ && memory_donor_) {
            memory_donor_->reserveIdleRomArena();
            borrowed_rom_arena_bytes_ = 0;
        }
    }
    ESP_LOGI(TAG, "%s startup complete: pogopo_os stack minimum free=%uB",
             displayTitle(), static_cast<unsigned>(
                 uxTaskGetStackHighWaterMark(nullptr)));
    context.invalidate();
}

void PogoDateApp::onExit(AppContext&) {
    runtime_.stop();
    if (borrowed_rom_arena_bytes_ && memory_donor_) {
        const uint32_t restored = memory_donor_->reserveIdleRomArena();
        ESP_LOGI(TAG, "PogoDate fast-RAM returned: restored=%luB",
                 static_cast<unsigned long>(restored));
        borrowed_rom_arena_bytes_ = 0;
    }
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
        context.launch("playdate_library");
        return;
    }
    if (event.type == input::EventType::Pressed) {
        queued_pressed_ = static_cast<input::ButtonMask>(
            queued_pressed_ | input::mask(event.button));
    }
}

void PogoDateApp::update(AppContext& context, uint32_t dt_ms) {
    if (start_error_ != ESP_OK || !runtime_.running()) return;

    const bool maze_package =
        std::strcmp(package_.bundle_id, "de.WuffderHundeheld.Maze") == 0;

    const imu::Sample motion = context.imu.sample();
    if (motion.valid && motion.sequence != accelerometer_sequence_) {
        accelerometer_sequence_ = motion.sequence;

        // BMI270 is mounted with sensor +Y pointing toward the top of the
        // display. Playdate's public coordinate system uses +Y toward the
        // bottom. Pogopo's BMI270 also reports gravity with the opposite Z
        // sign: a console lying flat on its back reads roughly -1 g, while
        // Playdate specifies +1 g for that pose. A short
        // one-sample IIR removes 50 Hz sensor chatter without the sluggish
        // 170 ms visual filtering used by Motion Lab.
        // Keep the board's established X mapping. STEP11.6.7 negated X based
        // on one title's observed direction, but that global experiment
        // inverted motion for every PDX and invalidated existing calibration.
        const float mapped_x = std::clamp(motion.ax, -2.0f, 2.0f);
        // Maze interprets its pitch in the opposite direction from the other
        // validated packages. Correct only its Y input so lowering the bottom
        // edge rolls the marble down without changing Godspeed, Duel, Celeste
        // or future PDX controls.
        const float mapped_y = std::clamp(
            maze_package ? motion.ay : -motion.ay, -2.0f, 2.0f);
        const float mapped_z = std::clamp(-motion.az, -2.0f, 2.0f);
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
    // Maze uses A for all confirmations.  START was only a Pogopo convenience
    // alias, but on the expensive completion scene it could queue a second A
    // and immediately activate the next screen.  Keep the alias for the other
    // validated packages and let Maze receive only its real A button.
    const bool start_alias = !maze_package;
    runtime_.setInput(
        playdateButtons(context.input.heldMask(), maze_package, start_alias),
        playdateButtons(queued_pressed_, maze_package, start_alias));
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
        const uint64_t profiled_game_us =
            static_cast<uint64_t>(stats.last_sprite_us) +
            stats.last_timer_us + stats.last_frame_timer_us;
        const uint32_t other_game_us = static_cast<uint32_t>(
            stats.last_game_us > profiled_game_us
                ? stats.last_game_us - profiled_game_us : 0U);
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
            "logic=%luus game=%luus spr=%luus timer=%luus ft=%luus "
            "other=%luus blit=%luus rows=%u/240 tx=%luB/%luus "
            "luaheap=%lu peak=%lu gc=%lu err=%lu RAM=%lu/%lu PSRAM=%lu "
            "acc=%+.2f,%+.2f,%+.2f i2cerr=%lu",
            displayTitle(), static_cast<unsigned long>(lua_fps),
            static_cast<unsigned long>(lcd_fps),
            static_cast<unsigned long>(stats.requested_fps),
            static_cast<unsigned long>(stats.last_update_us),
            static_cast<unsigned long>(stats.max_update_us),
            static_cast<unsigned long>(stats.last_logic_us),
            static_cast<unsigned long>(stats.last_game_us),
            static_cast<unsigned long>(stats.last_sprite_us),
            static_cast<unsigned long>(stats.last_timer_us),
            static_cast<unsigned long>(stats.last_frame_timer_us),
            static_cast<unsigned long>(other_game_us),
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
    menu::PogoFont::drawText(canvas, 39, 61, failure,
                             menu::FontFace::Italic14,
                             context.theme.foreground);
    char status[96]{};
    std::snprintf(status, sizeof(status), "%s", runtime_.error());
    menu::PogoFont::drawText(canvas, 39, 101, status,
                             menu::FontFace::Regular14,
                             context.theme.foreground);
    menu::PogoFont::drawText(canvas, 39, 145,
                             "Check serial for full diagnostic",
                             menu::FontFace::Regular14,
                             context.theme.foreground);
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
    list_.setRowHeight(29);
    rescan(context);
    enter_elapsed_ms_ = 0;
    redraw_elapsed_ms_ = 0;
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
        // The custom focus pill extends beyond gui::List::bounds(). Redraw all
        // rows so the old black selection never trails on the Sharp LCD.
        context.invalidate({0, 32, 400, 178});
    } else if (event.button == input::Button::Down && list_.move(1)) {
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate({0, 32, 400, 178});
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

void PogoDateBrowserApp::update(AppContext& context, uint32_t dt_ms) {
    const uint32_t previous_enter = enter_elapsed_ms_;
    enter_elapsed_ms_ = std::min<uint32_t>(enter_elapsed_ms_ + dt_ms, 330U);
    redraw_elapsed_ms_ += dt_ms;
    if (previous_enter < 330U && enter_elapsed_ms_ == 330U) {
        redraw_elapsed_ms_ = 0;
        context.invalidate();
    } else if (enter_elapsed_ms_ < 330U && redraw_elapsed_ms_ >= 32U) {
        redraw_elapsed_ms_ %= 32U;
        context.invalidate();
    } else if (enter_elapsed_ms_ >= 330U && redraw_elapsed_ms_ >= 500U) {
        redraw_elapsed_ms_ %= 500U;
        context.invalidate();
    }
}

void PogoDateBrowserApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(gfx::WHITE);
    const float raw_progress = std::clamp<float>(
        static_cast<float>(enter_elapsed_ms_) / 330.0f, 0.0f, 1.0f);
    const float progress = 1.0f - std::pow(1.0f - raw_progress, 3.0f);
    const int content_x = static_cast<int>(
        std::lround((1.0f - progress) * 400.0f));

    menu::PogoFont::drawText(canvas, 12 + content_x, -1, "playdate",
                             menu::FontFace::Italic22);
    const power::State power = context.power.state();
    canvas.draw_rect(366, 8, 23, 10, gfx::BLACK);
    canvas.fill_rect(389, 11, 2, 4, gfx::BLACK);
    const int battery_level = power.battery_valid
        ? std::clamp<int>((power.battery_percent + 24) / 25, 0, 4) : 0;
    for (int segment = 0; segment < battery_level; ++segment) {
        canvas.fill_rect(368 + segment * 5, 10, 4, 6, gfx::BLACK);
    }

    const size_t count = package_count_ ? package_count_ : 1;
    const int selected = static_cast<int>(list_.selected());
    constexpr int visible_rows = 7;
    const int first = std::clamp(selected - 3, 0,
        std::max(0, static_cast<int>(count) - visible_rows));
    for (int visible = 0;
         visible < visible_rows && first + visible < static_cast<int>(count);
         ++visible) {
        const int item_index = first + visible;
        const int y = 35 + visible * 25;
        const bool focus = item_index == selected;
        const auto& item = items_[item_index];
        if (focus) {
            canvas.fill_rect(9 + content_x, y + 1, 382, 22, gfx::BLACK);
            canvas.fill_circle(20 + content_x, y + 12, 11, gfx::BLACK);
            canvas.fill_circle(379 + content_x, y + 12, 11, gfx::BLACK);
        }

        const menu::FontFace face = focus
            ? menu::FontFace::Italic14 : menu::FontFace::Regular14;
        char subtitle[48]{};
        if (package_count_ && item_index < static_cast<int>(package_count_)) {
            std::snprintf(subtitle, sizeof(subtitle), "%s",
                          subtitles_[item_index].data());
            while (subtitle[0] && menu::PogoFont::textWidth(face, subtitle) > 190) {
                const size_t length = std::strlen(subtitle);
                if (length <= 3) break;
                subtitle[length - 1] = '\0';
            }
        }
        const int subtitle_width = menu::PogoFont::textWidth(face, subtitle);
        const int label_width = std::max(80, 350 - subtitle_width);
        char label[64]{};
        std::snprintf(label, sizeof(label), "%s", item.label ? item.label : "");
        while (label[0] && menu::PogoFont::textWidth(face, label) > label_width) {
            const size_t length = std::strlen(label);
            if (length <= 3) break;
            label[length - 1] = '\0';
        }
        menu::PogoFont::drawText(canvas, 16 + content_x, y, label, face,
                                 focus ? gfx::WHITE : gfx::BLACK);
        if (subtitle[0]) {
            menu::PogoFont::drawText(canvas,
                383 - subtitle_width + content_x, y, subtitle, face,
                focus ? gfx::WHITE : gfx::BLACK);
        }
    }

    char status[96]{};
    std::snprintf(status, sizeof(status), "%s", status_);
    while (status[0] &&
           menu::PogoFont::textWidth(menu::FontFace::Italic14, status) > 374) {
        const size_t length = std::strlen(status);
        if (length <= 3) break;
        status[length - 1] = '\0';
    }
    menu::PogoFont::drawText(canvas, 13 + content_x, 205, status,
                             menu::FontFace::Italic14);
    menu::PogoFont::drawText(canvas, 13 + content_x, 220,
                             "A play  B back  START rescan",
                             menu::FontFace::Regular14);
}

} // namespace pogopo::demo
