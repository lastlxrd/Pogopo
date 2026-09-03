#include "apps/demo_apps.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace pogopo::demo {

namespace {
struct HomeItem {
    const char* label;
    const char* target;
};

constexpr HomeItem kHomeItems[] = {
    {"pogopo", "pogopo_library"},
    {"gameboy", "gb_browser"},
    {"playdate", "playdate_library"},
    {"settings", "settings"},
};

constexpr int kHomeItemCount = static_cast<int>(sizeof(kHomeItems) / sizeof(kHomeItems[0]));
constexpr int kHomeRowY = 22;
constexpr int kHomeRowStep = 52;
constexpr uint32_t kEnterDurationMs = 420;
constexpr uint32_t kSwitchDurationMs = 280;
constexpr uint32_t kGameBoyPlaydateDurationMs = 360;
constexpr uint32_t kOpenDurationMs = 260;

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

float smooth_step(float value) {
    value = clamp01(value);
    return value * value * (3.0f - 2.0f * value);
}

float ease_out_cubic(float value) {
    value = 1.0f - clamp01(value);
    return 1.0f - value * value * value;
}

int interpolate(int from, int to, float progress) {
    return static_cast<int>(std::lround(
        static_cast<float>(from) + static_cast<float>(to - from) * progress));
}

bool gameboy_playdate_pair(int first, int second) {
    return (first == 1 && second == 2) || (first == 2 && second == 1);
}

void fill_pill(gfx::Canvas& canvas, int x, int y, int width, int height,
               gfx::Color color) {
    if (width <= 0 || height <= 0) return;
    const int radius = std::min(height / 2, width / 2);
    canvas.fill_rect(x + radius, y, width - radius * 2, height, color);
    canvas.fill_circle(x + radius, y + height / 2, radius, color);
    canvas.fill_circle(x + width - radius - 1, y + height / 2, radius, color);
}

void draw_battery(gfx::Canvas& canvas, const power::State& state,
                  uint32_t elapsed_ms, int x = 7, int y = 7) {
    canvas.draw_rect(x, y, 23, 10, gfx::BLACK);
    canvas.fill_rect(x + 23, y + 3, 2, 4, gfx::BLACK);
    int level = state.battery_valid
        ? std::clamp<int>((static_cast<int>(state.battery_percent) + 24) / 25, 0, 4)
        : 0;
    if (state.charging) {
        level = 1 + static_cast<int>((elapsed_ms / 240U) % 4U);
    }
    for (int segment = 0; segment < level; ++segment) {
        canvas.fill_rect(x + 2 + segment * 5, y + 2, 4, 6, gfx::BLACK);
    }
}

constexpr gui::ListItem kHapticItems[] = {
    {"Tick", "Small cursor feedback", "tick", true},
    {"Click", "Normal action", "click", true},
    {"Double click", "Two short pulses", "double", true},
    {"Confirm", "Positive confirmation", "confirm", true},
    {"Alert", "Attention pattern", "alert", true},
    {"Heavy", "Strong combo effect", "heavy", true},
};


constexpr gui::ListItem kAudioItems[] = {
    {"UI tick", "Short navigation sound", "tick", true},
    {"Click", "Normal action sound", "click", true},
    {"Confirm", "Two-note confirmation", "confirm", true},
    {"Back", "Descending back sound", "back", true},
    {"Error", "Low warning pattern", "error", true},
    {"Startup", "Three-note boot melody", "startup", true},
    {"Coin", "Arcade-style effect", "coin", true},
    {"440 Hz sine", "Raw tone generator", "sine440", true},
    {"880 Hz square", "Raw tone generator", "square880", true},
    {"Noise burst", "Mixer noise voice", "noise", true},
};

constexpr uint8_t kSmileData[] = {
    0x03,0xC0, 0x0F,0xF0, 0x1C,0x38, 0x30,0x0C,
    0x63,0xC6, 0x47,0xE2, 0xC6,0x63, 0xC0,0x03,
    0xC0,0x03, 0xC4,0x23, 0x66,0x66, 0x33,0xCC,
    0x1C,0x38, 0x0F,0xF0, 0x03,0xC0, 0x00,0x00,
};
const gfx::Bitmap kSmile = gfx::make_bitmap_1bpp(16, 16, kSmileData);

bool nav_event(const input::Event& event) {
    return event.type == input::EventType::Pressed || event.type == input::EventType::Repeat;
}

void draw_back_footer(AppContext& context, const char* extra = nullptr) {
    gui::draw_footer(context.gfx.canvas(), context.theme, "B BACK   MENU SYSTEM", extra);
}

void draw_button(gfx::Canvas& canvas, const gui::Theme& theme,
                 int x, int y, const char* label, bool pressed) {
    const gfx::Color bg = pressed ? theme.focus_background : theme.background;
    const gfx::Color fg = pressed ? theme.focus_foreground : theme.foreground;
    canvas.fill_rect(x, y, 42, 27, bg);
    canvas.draw_rect(x, y, 42, 27, theme.border);
    canvas.draw_text(gui::centered_text_x({x, y, 42, 27}, label), y + 10, label,
                     gfx::font5x7(), fg, 1, true, bg);
}
} // namespace

LauncherApp::LauncherApp() = default;

void LauncherApp::onEnter(AppContext& context) {
    previous_ = selected_;
    phase_ = Phase::Entering;
    phase_elapsed_ms_ = 0;
    redraw_elapsed_ms_ = 0;
    last_art_key_ = UINT16_MAX;
    last_battery_key_ = 0xFF;
    launch_target_ = nullptr;
    context.invalidate();
}

void LauncherApp::onEvent(AppContext& context, const input::Event& event) {
    if (phase_ != Phase::Idle || !nav_event(event)) return;
    if (event.button == input::Button::Top) {
        startSwitch(context, -1);
    } else if (event.button == input::Button::Down) {
        startSwitch(context, 1);
    } else if (event.type == input::EventType::Pressed &&
               event.button == input::Button::A) {
        launch_target_ = kHomeItems[selected_].target;
        phase_ = Phase::Opening;
        phase_elapsed_ms_ = 0;
        redraw_elapsed_ms_ = 0;
        context.haptics.play(haptics::Effect::Confirm);
        context.uiSound(audio::Effect::Confirm);
        context.invalidate();
    }
}

void LauncherApp::startSwitch(AppContext& context, int direction) {
    previous_ = selected_;
    direction_ = direction < 0 ? -1 : 1;
    selected_ = (selected_ + direction_ + kHomeItemCount) % kHomeItemCount;
    phase_ = Phase::Switching;
    phase_elapsed_ms_ = 0;
    redraw_elapsed_ms_ = 0;
    context.haptics.play(haptics::Effect::Tick);
    context.uiSound(audio::Effect::Tick);
    context.invalidate();
}

void LauncherApp::update(AppContext& context, uint32_t dt_ms) {
    art_elapsed_ms_ += dt_ms;
    redraw_elapsed_ms_ += dt_ms;
    if (phase_ != Phase::Idle) phase_elapsed_ms_ += dt_ms;

    if (phase_ == Phase::Entering && phase_elapsed_ms_ >= kEnterDurationMs) {
        phase_ = Phase::Idle;
        phase_elapsed_ms_ = kEnterDurationMs;
        last_art_key_ = UINT16_MAX;
        context.invalidate();
    } else if (phase_ == Phase::Switching && phase_elapsed_ms_ >=
               (gameboy_playdate_pair(previous_, selected_)
                    ? kGameBoyPlaydateDurationMs : kSwitchDurationMs)) {
        phase_ = Phase::Idle;
        phase_elapsed_ms_ = gameboy_playdate_pair(previous_, selected_)
            ? kGameBoyPlaydateDurationMs : kSwitchDurationMs;
        previous_ = selected_;
        last_art_key_ = UINT16_MAX;
        context.invalidate();
    } else if (phase_ == Phase::Opening && phase_elapsed_ms_ >= kOpenDurationMs) {
        const char* target = launch_target_;
        launch_target_ = nullptr;
        if (target) context.launch(target);
        return;
    }

    if (phase_ != Phase::Idle && redraw_elapsed_ms_ >= 32U) {
        redraw_elapsed_ms_ %= 32U;
        context.invalidate();
        return;
    }

    if (phase_ == Phase::Idle) {
        uint16_t art_key = 0;
        if (selected_ == 3) {
            art_key = static_cast<uint16_t>((art_elapsed_ms_ / 70U) & 0xFFFFU);
        } else {
            const menu::Art art = selected_ == 0 ? menu::Art::Pogopo
                                 : selected_ == 1 ? menu::Art::GameBoy
                                                  : menu::Art::Playdate;
            art_key = static_cast<uint16_t>(menu::Assets::frameAtTime(art, art_elapsed_ms_));
        }

        const power::State battery = context.power.state();
        uint8_t battery_key = battery.battery_valid
            ? static_cast<uint8_t>(std::clamp<int>((battery.battery_percent + 24) / 25, 0, 4))
            : 0;
        if (battery.charging) {
            battery_key = static_cast<uint8_t>(0x10U | ((art_elapsed_ms_ / 240U) & 0x03U));
        }
        if (art_key != last_art_key_ || battery_key != last_battery_key_) {
            last_art_key_ = art_key;
            last_battery_key_ = battery_key;
            context.invalidate();
        }
    }
}

void LauncherApp::drawSettingsArt(AppContext& context, int y_offset,
                                  uint32_t elapsed_ms) {
    auto& canvas = context.gfx.canvas();
    const int x = 162;
    const int y = 28 + y_offset;
    canvas.draw_rect(x, y, 222, 183, gfx::BLACK);
    menu::PogoFont::drawText(canvas, x + 13, y + 5, "system status",
                             menu::FontFace::Italic14);
    canvas.draw_hline(x + 10, y + 30, 202, gfx::BLACK);

    const char* labels[] = {"display", "audio", "storage", "motion"};
    const bool states[] = {
        context.gfx.ok(), context.audio.ok(), context.storage.mounted(), context.imu.ok(),
    };
    for (int row = 0; row < 4; ++row) {
        const int row_y = y + 43 + row * 28;
        menu::PogoFont::drawText(canvas, x + 13, row_y, labels[row],
                                 menu::FontFace::Regular14);
        canvas.draw_rect(x + 151, row_y + 5, 50, 9, gfx::BLACK);
        const int sweep = static_cast<int>((elapsed_ms / 70U + row * 7U) % 45U);
        const int amount = states[row] ? 44 : std::max(4, sweep / 3);
        canvas.fill_rect(x + 154, row_y + 8, amount, 3, gfx::BLACK);
    }
    const int pulse = 4 + static_cast<int>((elapsed_ms / 90U) % 12U);
    canvas.draw_circle(x + 190, y + 164, pulse, gfx::BLACK);
    canvas.fill_circle(x + 190, y + 164, 2, gfx::BLACK);
}

void LauncherApp::drawArt(AppContext& context, int item, int y_offset,
                          uint32_t elapsed_ms) {
    if (item == 3) {
        drawSettingsArt(context, y_offset, elapsed_ms);
        return;
    }

    const menu::Art art = item == 0 ? menu::Art::Pogopo
                         : item == 1 ? menu::Art::GameBoy
                                     : menu::Art::Playdate;
    const auto info = menu::Assets::info(art);
    const size_t frame = menu::Assets::frameAtTime(art, elapsed_ms);
    context.gfx.canvas().draw_bitmap(
        info.source_x, info.source_y + y_offset, menu::Assets::frame(art, frame));
}

void LauncherApp::drawGameBoyRoll(AppContext& context, float progress,
                                  bool entering) {
    const menu::Art art = menu::Art::GameBoyFull;
    const auto info = menu::Assets::info(art);
    const size_t frame = menu::Assets::frameAtTime(art, art_elapsed_ms_);
    constexpr float reveal_end = 0.48f;
    const int top_aligned = info.source_y;
    const int bottom_aligned = context.gfx.height() - info.height;
    const int fully_above = -info.height - 4;

    // The supplied 251x419 console remains at its native size. The transition
    // first exposes its lower half, then moves that last half out quickly before
    // Playdate arrives. Entering Game Boy evaluates the exact reverse path.
    const auto leaving_y = [&](float value) {
        if (value <= reveal_end) {
            return interpolate(top_aligned, bottom_aligned,
                               smooth_step(value / reveal_end));
        }
        return interpolate(bottom_aligned, fully_above,
                           smooth_step((value - reveal_end) / (1.0f - reveal_end)));
    };

    if (entering) {
        const float playdate_exit_end = 1.0f - reveal_end;
        if (progress < playdate_exit_end) {
            drawArt(context, 2,
                    interpolate(0, context.gfx.height(),
                                smooth_step(progress / playdate_exit_end)),
                    art_elapsed_ms_);
        }
    } else if (progress > reveal_end) {
        drawArt(context, 2,
                interpolate(context.gfx.height(), 0,
                            smooth_step((progress - reveal_end) / (1.0f - reveal_end))),
                art_elapsed_ms_);
    }

    const int y = entering ? leaving_y(1.0f - progress) : leaving_y(progress);
    context.gfx.canvas().draw_bitmap(
        386 - info.width, y, menu::Assets::frame(art, frame));
}

void LauncherApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(gfx::WHITE);

    float phase_progress = 1.0f;
    int menu_x = 5;
    if (phase_ == Phase::Entering) {
        phase_progress = ease_out_cubic(
            static_cast<float>(phase_elapsed_ms_) / kEnterDurationMs);
        menu_x = interpolate(-140, 5, phase_progress);
    }

    float switch_progress = 1.0f;
    int pill_y = kHomeRowY + selected_ * kHomeRowStep + 2;
    const char* pill_label = kHomeItems[selected_].label;
    if (phase_ == Phase::Switching) {
        const uint32_t duration = gameboy_playdate_pair(previous_, selected_)
            ? kGameBoyPlaydateDurationMs : kSwitchDurationMs;
        switch_progress = smooth_step(
            static_cast<float>(phase_elapsed_ms_) / duration);
        pill_y = interpolate(kHomeRowY + previous_ * kHomeRowStep + 2,
                             kHomeRowY + selected_ * kHomeRowStep + 2,
                             switch_progress);
        pill_label = switch_progress < 0.5f
            ? kHomeItems[previous_].label : kHomeItems[selected_].label;
    }

    // Art is the moving background layer. Text, focus pill and battery are
    // drawn afterwards so their pixels always stay crisp in the foreground.
    if (phase_ == Phase::Entering) {
        drawArt(context, selected_, interpolate(240, 0, phase_progress), art_elapsed_ms_);
    } else if (phase_ == Phase::Switching) {
        if (gameboy_playdate_pair(previous_, selected_)) {
            const float raw_progress = clamp01(
                static_cast<float>(phase_elapsed_ms_) / kGameBoyPlaydateDurationMs);
            drawGameBoyRoll(context, raw_progress, selected_ == 1);
        } else {
            const int old_offset = interpolate(0, -direction_ * 240, switch_progress);
            const int new_offset = interpolate(direction_ * 240, 0, switch_progress);
            drawArt(context, previous_, old_offset, art_elapsed_ms_);
            drawArt(context, selected_, new_offset, art_elapsed_ms_);
        }
    } else {
        drawArt(context, selected_, 0, art_elapsed_ms_);
    }

    for (int item = 0; item < kHomeItemCount; ++item) {
        menu::PogoFont::drawText(canvas, menu_x, kHomeRowY + item * kHomeRowStep,
                                 kHomeItems[item].label, menu::FontFace::Regular24);
    }

    const int pill_width = menu::PogoFont::textWidth(menu::FontFace::Italic24, pill_label) + 17;
    fill_pill(canvas, menu_x - 2, pill_y, pill_width, 34, gfx::BLACK);
    menu::PogoFont::drawText(canvas, menu_x + 5, pill_y - 3, pill_label,
                             menu::FontFace::Italic24, gfx::WHITE);
    draw_battery(canvas, context.power.state(), art_elapsed_ms_);

    if (phase_ == Phase::Opening) {
        const float open_progress = smooth_step(
            static_cast<float>(phase_elapsed_ms_) / kOpenDurationMs);
        const int wipe_x = interpolate(400, -1, open_progress);
        canvas.fill_rect(wipe_x, 0, 400 - wipe_x, 240, gfx::WHITE);
        if (wipe_x >= 0 && wipe_x < 400) canvas.draw_vline(wipe_x, 0, 240, gfx::BLACK);
    }
}

void EmptyLibraryApp::onEnter(AppContext& context) {
    elapsed_ms_ = 0;
    redraw_elapsed_ms_ = 0;
    enter_elapsed_ms_ = 0;
    last_frame_ = static_cast<size_t>(-1);
    context.invalidate();
}

void EmptyLibraryApp::onEvent(AppContext& context, const input::Event& event) {
    if (event.type != input::EventType::Pressed) return;
    if (event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Back);
        context.home();
    } else if (event.button == input::Button::A) {
        context.haptics.play(haptics::Effect::Alert);
        context.uiSound(audio::Effect::Error);
    }
}

void EmptyLibraryApp::update(AppContext& context, uint32_t dt_ms) {
    elapsed_ms_ += dt_ms;
    enter_elapsed_ms_ = std::min<uint32_t>(enter_elapsed_ms_ + dt_ms, 320U);
    redraw_elapsed_ms_ += dt_ms;
    if (enter_elapsed_ms_ < 320U && redraw_elapsed_ms_ >= 32U) {
        redraw_elapsed_ms_ %= 32U;
        context.invalidate();
        return;
    }
    const size_t frame = menu::Assets::frameAtTime(art_, elapsed_ms_);
    if (frame != last_frame_) {
        last_frame_ = frame;
        context.invalidate();
    } else if (redraw_elapsed_ms_ >= 500U) {
        redraw_elapsed_ms_ %= 500U;
        context.invalidate();
    }
}

void EmptyLibraryApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(gfx::WHITE);
    const float progress = ease_out_cubic(static_cast<float>(enter_elapsed_ms_) / 320.0f);
    const int content_x = interpolate(400, 0, progress);
    draw_battery(canvas, context.power.state(), elapsed_ms_);

    menu::PogoFont::drawText(canvas, 14 + content_x, 31, platform_name_,
                             menu::FontFace::Italic22);
    canvas.draw_hline(14 + content_x, 67, 114, gfx::BLACK);
    menu::PogoFont::drawText(canvas, 14 + content_x, 80, "no games yet",
                             menu::FontFace::Regular14);
    menu::PogoFont::drawText(canvas, 14 + content_x, 103, "folder:",
                             menu::FontFace::Regular14);
    menu::PogoFont::drawText(canvas, 14 + content_x, 122, folder_,
                             menu::FontFace::Italic14);
    menu::PogoFont::drawText(canvas, 14 + content_x, 196, "B  back",
                             menu::FontFace::Regular14);

    const auto info = menu::Assets::info(art_);
    const size_t frame = menu::Assets::frameAtTime(art_, elapsed_ms_);
    canvas.draw_bitmap(info.source_x + content_x, info.source_y,
                       menu::Assets::frame(art_, frame));
}

void GraphicsDemoApp::onEnter(AppContext& context) {
    x_ = 35;
    previous_x_ = x_;
    velocity_ = 3;
    accumulator_ms_ = 0;
    paused_ = false;
    frame_counter_ = 0;
    context.invalidate();
}

void GraphicsDemoApp::onEvent(AppContext& context, const input::Event& event) {
    if (event.type != input::EventType::Pressed) return;
    if (event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Back);
        context.launch("settings");
    } else if (event.button == input::Button::A) {
        paused_ = !paused_;
        context.haptics.play(paused_ ? haptics::Effect::DoubleClick : haptics::Effect::Confirm);
        context.uiSound(paused_ ? audio::Effect::Click : audio::Effect::Confirm);
        context.invalidate({12, 27, 376, 196});
    }
}

void GraphicsDemoApp::update(AppContext& context, uint32_t dt_ms) {
    if (paused_) return;
    accumulator_ms_ += dt_ms;
    if (accumulator_ms_ < 32) return;
    accumulator_ms_ = 0;
    previous_x_ = x_;
    x_ += velocity_;
    if (x_ <= 24 || x_ >= 360) {
        x_ = std::clamp(x_, 24, 360);
        velocity_ = -velocity_;
    }
    ++frame_counter_;
    const int left = std::min(previous_x_, x_) - 2;
    const int right = std::max(previous_x_, x_) + 18;
    context.invalidate({left, 88, right - left, 18});
    // Keep normal animation frames tiny. Refresh the progress/stat area about once a second.
    if ((frame_counter_ % 31U) == 0U) {
        context.invalidate({20, 177, 360, 32});
    }
}

void GraphicsDemoApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(context.theme.background);
    gui::draw_header(canvas, context.theme, "GRAPHICS DEMO", paused_ ? "PAUSED" : "RUNNING");
    canvas.draw_rect(18, 44, 364, 118, context.theme.border);
    canvas.draw_text(30, 55, "PARTIAL REDRAW + GUI WIDGETS", gfx::font5x7(), context.theme.foreground);
    canvas.draw_line(32, 75, 365, 75, context.theme.foreground);
    canvas.draw_bitmap(x_, 88, kSmile, context.theme.foreground, true, context.theme.background);
    canvas.draw_circle(78, 135, 16, context.theme.foreground);
    canvas.fill_circle(130, 135, 13, context.theme.foreground);
    canvas.draw_rect(172, 120, 44, 30, context.theme.foreground);
    canvas.fill_rect(236, 120, 44, 30, context.theme.foreground);

    gui::ProgressBar progress({22, 178, 356, 18});
    progress.setRange(24, 360);
    progress.setValue(x_);
    progress.draw(canvas, context.theme);

    char status[64];
    const auto stats = context.gfx.stats();
    std::snprintf(status, sizeof(status), "FRAME %u  ROWS %u  BYTES %u",
                  static_cast<unsigned>(frame_counter_),
                  static_cast<unsigned>(stats.last_rows),
                  static_cast<unsigned>(stats.last_bytes));
    canvas.draw_text(24, 202, status, gfx::font5x7(), context.theme.foreground);
    draw_back_footer(context, "A PAUSE");
}

void InputMonitorApp::onEnter(AppContext& context) {
    last_held_ = 0xFF;
    last_raw_ = 0xFF;
    context.invalidate();
}

void InputMonitorApp::onEvent(AppContext& context, const input::Event& event) {
    if (event.type == input::EventType::Pressed && event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Back);
        context.launch("settings");
    }
}

void InputMonitorApp::update(AppContext& context, uint32_t) {
    const input::ButtonMask held = context.input.heldMask();
    const uint8_t raw = context.input.rawPort();
    if (held != last_held_ || raw != last_raw_) {
        last_held_ = held;
        last_raw_ = raw;
        context.invalidate({18, 40, 364, 168});
    }
}

void InputMonitorApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(context.theme.background);
    gui::draw_header(canvas, context.theme, "INPUT MONITOR", context.input.ok() ? "TCA OK" : "ERROR");
    const input::ButtonMask held = context.input.heldMask();

    draw_button(canvas, context.theme, 179, 46, "TOP",   held & input::mask(input::Button::Top));
    draw_button(canvas, context.theme, 128, 79, "LEFT", held & input::mask(input::Button::Left));
    draw_button(canvas, context.theme, 179, 79, "DOWN", held & input::mask(input::Button::Down));
    draw_button(canvas, context.theme, 230, 79, "RIGHT",held & input::mask(input::Button::Right));
    draw_button(canvas, context.theme, 292, 58, "B",    held & input::mask(input::Button::B));
    draw_button(canvas, context.theme, 343, 46, "A",    held & input::mask(input::Button::A));
    draw_button(canvas, context.theme, 104, 130, "MENU",held & input::mask(input::Button::Menu));
    draw_button(canvas, context.theme, 254, 130, "START",held & input::mask(input::Button::Start));

    char line[96];
    std::snprintf(line, sizeof(line), "HELD 0x%02X   RAW 0x%02X", held, context.input.rawPort());
    canvas.draw_text(25, 177, line, gfx::font5x7(), context.theme.foreground);
    std::snprintf(line, sizeof(line), "DROPPED %u   I2C ERR %u",
                  static_cast<unsigned>(context.input.droppedEvents()),
                  static_cast<unsigned>(context.input.readErrors()));
    canvas.draw_text(25, 193, line, gfx::font5x7(), context.theme.foreground);
    draw_back_footer(context);
}

HapticsLabApp::HapticsLabApp() {
    list_.setItems(kHapticItems, sizeof(kHapticItems) / sizeof(kHapticItems[0]));
    list_.setRowHeight(24);
}

void HapticsLabApp::onEnter(AppContext& context) {
    last_effect_ = "READY";
    context.invalidate();
}

void HapticsLabApp::onEvent(AppContext& context, const input::Event& event) {
    if (!nav_event(event)) return;
    if (event.button == input::Button::Top && list_.move(-1)) {
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate(list_.bounds());
    } else if (event.button == input::Button::Down && list_.move(1)) {
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate(list_.bounds());
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Back);
        context.launch("settings");
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::A) {
        const size_t selected = list_.selected();
        static constexpr haptics::Effect effects[] = {
            haptics::Effect::Tick, haptics::Effect::Click, haptics::Effect::DoubleClick,
            haptics::Effect::Confirm, haptics::Effect::Alert, haptics::Effect::Heavy,
        };
        if (selected < sizeof(effects) / sizeof(effects[0])) {
            context.haptics.play(effects[selected]);
            context.uiSound(audio::Effect::Click);
            last_effect_ = haptics::effect_name(effects[selected]);
            context.invalidate({20, 198, 360, 20});
        }
    }
}

void HapticsLabApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(context.theme.background);
    gui::draw_header(canvas, context.theme, "HAPTICS LAB", context.haptics.active() ? "MOTOR ON" : "READY");
    list_.draw(canvas, context.theme);
    char status[64];
    std::snprintf(status, sizeof(status), "LAST: %s   DROP: %u", last_effect_,
                  static_cast<unsigned>(context.haptics.droppedCommands()));
    canvas.draw_text(24, 202, status, gfx::font5x7(), context.theme.foreground);
    draw_back_footer(context, "A PLAY");
}


AudioLabApp::AudioLabApp() {
    list_.setItems(kAudioItems, sizeof(kAudioItems) / sizeof(kAudioItems[0]));
    list_.setRowHeight(24);
}

void AudioLabApp::onEnter(AppContext& context) {
    last_sound_ = "READY";
    stats_accumulator_ms_ = 0;
    const auto stats = context.audio.stats();
    last_buffers_ = stats.buffers_written;
    last_voices_ = stats.active_voices;
    context.invalidate();
}

void AudioLabApp::onEvent(AppContext& context, const input::Event& event) {
    if (!nav_event(event)) return;

    if (event.button == input::Button::Top && list_.move(-1)) {
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate(list_.bounds());
    } else if (event.button == input::Button::Down && list_.move(1)) {
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate(list_.bounds());
    } else if (event.button == input::Button::Left || event.button == input::Button::Right) {
        const int delta = event.button == input::Button::Right ? 5 : -5;
        const int volume = std::clamp<int>(static_cast<int>(context.audio.masterVolume()) + delta, 0, 100);
        context.audio.setMasterVolume(static_cast<uint8_t>(volume));
        context.settings.setVolume(static_cast<uint8_t>(volume));
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Click);
        context.invalidate({20, 176, 360, 39});
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Back);
        context.launch("settings");
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::Start) {
        context.haptics.play(haptics::Effect::Confirm);
        context.audio.play(audio::Effect::Startup);
        last_sound_ = "STARTUP";
        context.invalidate({20, 197, 360, 18});
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::A) {
        const size_t selected = list_.selected();
        static constexpr audio::Effect effects[] = {
            audio::Effect::Tick,
            audio::Effect::Click,
            audio::Effect::Confirm,
            audio::Effect::Back,
            audio::Effect::Error,
            audio::Effect::Startup,
            audio::Effect::Coin,
        };

        bool queued = false;
        if (selected < sizeof(effects) / sizeof(effects[0])) {
            queued = context.audio.play(effects[selected]);
            last_sound_ = audio::effect_name(effects[selected]);
        } else if (selected == 7) {
            queued = context.audio.tone(440, 450, 72, audio::Waveform::Sine);
            last_sound_ = "440 SINE";
        } else if (selected == 8) {
            queued = context.audio.tone(880, 260, 62, audio::Waveform::Square);
            last_sound_ = "880 SQUARE";
        } else if (selected == 9) {
            queued = context.audio.tone(1000, 300, 48, audio::Waveform::Noise);
            last_sound_ = "NOISE";
        }

        context.haptics.play(queued ? haptics::Effect::Click : haptics::Effect::Alert);
        if (!queued) last_sound_ = "QUEUE FULL";
        context.invalidate({20, 197, 360, 18});
    }
}

void AudioLabApp::update(AppContext& context, uint32_t dt_ms) {
    stats_accumulator_ms_ += dt_ms;
    if (stats_accumulator_ms_ < 100) return;
    stats_accumulator_ms_ = 0;

    const auto stats = context.audio.stats();
    if (stats.buffers_written != last_buffers_ || stats.active_voices != last_voices_) {
        last_buffers_ = stats.buffers_written;
        last_voices_ = stats.active_voices;
        context.invalidate({20, 197, 360, 18});
    }
}

void AudioLabApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(context.theme.background);
    gui::draw_header(canvas, context.theme, "AUDIO LAB", context.audio.ok() ? "I2S 32768" : "ERROR");
    list_.draw(canvas, context.theme);

    gui::ProgressBar volume({22, 179, 356, 14});
    volume.setRange(0, 100);
    volume.setValue(context.audio.masterVolume());
    volume.draw(canvas, context.theme);

    const auto stats = context.audio.stats();
    char status[96];
    std::snprintf(status, sizeof(status), "VOL %u%%  %s  V%u E%u D%u",
                  static_cast<unsigned>(context.audio.masterVolume()), last_sound_,
                  static_cast<unsigned>(stats.active_voices),
                  static_cast<unsigned>(stats.write_errors),
                  static_cast<unsigned>(stats.dropped_commands));
    canvas.draw_text(24, 199, status, gfx::font5x7(), context.theme.foreground);
    gui::draw_footer(canvas, context.theme, "A PLAY  L/R VOL  B BACK", "START SONG");
}


void WavPlayerApp::rescan(AppContext& context) {
    file_count_ = context.storage.listWav("/pogopo/sounds", files_.data(), files_.size());
    for (size_t i = 0; i < file_count_; ++i) {
        const uint32_t size_kb = (files_[i].size + 1023U) / 1024U;
        if (size_kb >= 1024U) {
            std::snprintf(subtitles_[i].data(), subtitles_[i].size(), "%lu.%lu MB",
                          static_cast<unsigned long>(size_kb / 1024U),
                          static_cast<unsigned long>((size_kb % 1024U) / 103U));
        } else {
            std::snprintf(subtitles_[i].data(), subtitles_[i].size(), "%lu KB",
                          static_cast<unsigned long>(size_kb));
        }
        items_[i] = {files_[i].name, subtitles_[i].data(), files_[i].path, true};
    }
    list_.setItems(items_.data(), file_count_);
    list_.setRowHeight(27);
    std::snprintf(status_, sizeof(status_), file_count_ ? "%u WAV FILES - STREAM READY" : "NO /pogopo/sounds/*.wav",
                  static_cast<unsigned>(file_count_));
    context.invalidate();
}

void WavPlayerApp::onEnter(AppContext& context) {
    rescan(context);
    ui_accumulator_ms_ = 0;
    const auto info = context.audio.streamInfo();
    last_state_ = info.state;
    last_position_second_ = info.position_ms / 1000U;
}

void WavPlayerApp::startSelected(AppContext& context) {
    if (!context.storage.mounted() || file_count_ == 0) {
        std::snprintf(status_, sizeof(status_), "SD/WAV NOT FOUND");
        context.haptics.play(haptics::Effect::Alert);
        context.uiSound(audio::Effect::Error);
        return;
    }

    const auto& file = files_[list_.selected()];
    const bool queued = context.audio.playStream(file.path, 82);
    if (queued) {
        std::snprintf(status_, sizeof(status_), "OPENING %.52s", file.name);
        context.haptics.play(haptics::Effect::Confirm);
        context.uiSound(audio::Effect::Confirm);
    } else {
        std::snprintf(status_, sizeof(status_), context.audio.enabled() ? "STREAM QUEUE FULL" : "AUDIO DISABLED");
        context.haptics.play(haptics::Effect::Alert);
        context.uiSound(audio::Effect::Error);
    }
}

void WavPlayerApp::onEvent(AppContext& context, const input::Event& event) {
    if (!nav_event(event)) return;

    if (event.button == input::Button::Top && list_.move(-1)) {
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate(list_.bounds());
    } else if (event.button == input::Button::Down && list_.move(1)) {
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate(list_.bounds());
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Back);
        context.launch("settings"); // Music intentionally keeps playing in the background.
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::A) {
        startSelected(context);
        context.invalidate({18, 158, 364, 59});
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::Start) {
        if (!context.audio.toggleStreamPause()) {
            startSelected(context);
        } else {
            context.haptics.play(haptics::Effect::DoubleClick);
        }
        context.invalidate({18, 158, 364, 59});
    } else if (event.type == input::EventType::Pressed &&
               (event.button == input::Button::Left || event.button == input::Button::Right)) {
        const auto info = context.audio.streamInfo();
        if (context.audio.streamActive() || info.state == audio::StreamState::Finished) {
            int64_t target = static_cast<int64_t>(info.position_ms);
            target += event.button == input::Button::Right ? 5000 : -5000;
            target = std::clamp<int64_t>(target, 0, info.duration_ms);
            if (context.audio.seekStreamMs(static_cast<uint32_t>(target))) {
                context.haptics.play(haptics::Effect::Tick);
                context.uiSound(audio::Effect::Tick);
            }
        }
    }
}

void WavPlayerApp::update(AppContext& context, uint32_t dt_ms) {
    ui_accumulator_ms_ += dt_ms;
    if (ui_accumulator_ms_ < 125U) return;
    ui_accumulator_ms_ = 0;

    const auto info = context.audio.streamInfo();
    const uint32_t second = info.position_ms / 1000U;
    if (info.state != last_state_ || second != last_position_second_) {
        if (info.state == audio::StreamState::Error && last_state_ != audio::StreamState::Error) {
            context.haptics.play(haptics::Effect::Alert);
            context.uiSound(audio::Effect::Error);
        }
        last_state_ = info.state;
        last_position_second_ = second;
        context.invalidate({18, 158, 364, 59});
    }
}

void WavPlayerApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(context.theme.background);
    const auto info = context.audio.streamInfo();
    gui::draw_header(canvas, context.theme, "STREAM WAV", audio::stream_state_name(info.state));

    if (file_count_) {
        list_.draw(canvas, context.theme);
    } else {
        gui::draw_wrapped_text(canvas, {28, 58, 344, 82},
            "Copy WAV files to /pogopo/sounds on the SD card, then reopen this app.",
            context.theme, 1, 4);
    }

    gui::ProgressBar progress({22, 161, 356, 14});
    progress.setRange(0, static_cast<int>(std::max<uint32_t>(1U, info.duration_ms / 1000U)));
    progress.setValue(static_cast<int>(info.position_ms / 1000U));
    progress.draw(canvas, context.theme);

    char time_line[96];
    const uint32_t pos_s = info.position_ms / 1000U;
    const uint32_t dur_s = info.duration_ms / 1000U;
    std::snprintf(time_line, sizeof(time_line), "%02lu:%02lu / %02lu:%02lu  BUF %lums",
                  static_cast<unsigned long>(pos_s / 60U), static_cast<unsigned long>(pos_s % 60U),
                  static_cast<unsigned long>(dur_s / 60U), static_cast<unsigned long>(dur_s % 60U),
                  static_cast<unsigned long>(info.buffered_ms));
    canvas.draw_text(24, 181, time_line, gfx::font5x7(), context.theme.foreground);

    std::snprintf(status_, sizeof(status_), "%s %luHZ %u/%u  U%lu E%lu",
                  audio::stream_state_name(info.state),
                  static_cast<unsigned long>(info.sample_rate),
                  static_cast<unsigned>(info.channels),
                  static_cast<unsigned>(info.bits_per_sample),
                  static_cast<unsigned long>(info.underruns),
                  static_cast<unsigned long>(info.read_errors));
    canvas.draw_text(24, 197, status_, gfx::font5x7(), context.theme.foreground);
    gui::draw_footer(canvas, context.theme, "A PLAY  START PAUSE  B BACK", "L/R SEEK 5S");
}

namespace {

struct SettingsHubItem {
    const char* label;
    const char* target;
};

constexpr SettingsHubItem kSettingsHubItems[] = {
    {"preferences", "preferences"},
    {"controls test", "input"},
    {"audio test", "audio"},
    {"haptics test", "haptics"},
    {"motion & imu", "motion"},
    {"sd card & wav", "wav"},
    {"battery & usb", "power"},
    {"display test", "graphics"},
    {"about pogopo", "about"},
};

constexpr int kSettingsHubCount = static_cast<int>(
    sizeof(kSettingsHubItems) / sizeof(kSettingsHubItems[0]));

} // namespace

void SettingsApp::onEnter(AppContext& context) {
    enter_elapsed_ms_ = 0;
    status_elapsed_ms_ = 0;
    redraw_elapsed_ms_ = 0;
    context.invalidate();
}

void SettingsApp::onExit(AppContext&) {}

void SettingsApp::onEvent(AppContext& context, const input::Event& event) {
    if (!nav_event(event)) return;
    if (event.button == input::Button::Top) {
        selected_ = (selected_ + kSettingsHubCount - 1) % kSettingsHubCount;
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate();
    } else if (event.button == input::Button::Down) {
        selected_ = (selected_ + 1) % kSettingsHubCount;
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate();
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Back);
        context.home();
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::A) {
        context.haptics.play(haptics::Effect::Confirm);
        context.uiSound(audio::Effect::Confirm);
        context.launch(kSettingsHubItems[selected_].target);
    }
}

void SettingsApp::update(AppContext& context, uint32_t dt_ms) {
    const uint32_t previous_enter = enter_elapsed_ms_;
    enter_elapsed_ms_ = std::min<uint32_t>(enter_elapsed_ms_ + dt_ms, 340U);
    status_elapsed_ms_ += dt_ms;
    redraw_elapsed_ms_ += dt_ms;
    if (previous_enter < 340U && enter_elapsed_ms_ == 340U) {
        redraw_elapsed_ms_ = 0;
        context.invalidate();
    } else if (enter_elapsed_ms_ < 340U && redraw_elapsed_ms_ >= 32U) {
        redraw_elapsed_ms_ %= 32U;
        context.invalidate();
    } else if (enter_elapsed_ms_ >= 340U && redraw_elapsed_ms_ >= 240U) {
        redraw_elapsed_ms_ %= 240U;
        context.invalidate();
    }
}

void SettingsApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(gfx::WHITE);
    const float progress = ease_out_cubic(static_cast<float>(enter_elapsed_ms_) / 340.0f);
    const int content_x = interpolate(400, 0, progress);

    menu::PogoFont::drawText(canvas, 12 + content_x, -1, "settings",
                             menu::FontFace::Italic24);
    draw_battery(canvas, context.power.state(), status_elapsed_ms_, 366, 8);

    constexpr int visible_rows = 5;
    int first = std::clamp(selected_ - 2, 0, kSettingsHubCount - visible_rows);
    for (int visible = 0; visible < visible_rows; ++visible) {
        const int item = first + visible;
        const int y = 38 + visible * 35;
        const bool selected = item == selected_;

        if (selected) fill_pill(canvas, 9 + content_x, y, 382, 31, gfx::BLACK);
        const gfx::Color color = selected ? gfx::WHITE : gfx::BLACK;
        const menu::FontFace face = selected
            ? menu::FontFace::Italic22 : menu::FontFace::Regular22;
        menu::PogoFont::drawText(canvas, 17 + content_x, y - 1, kSettingsHubItems[item].label,
                                 face, color);
    }
    menu::PogoFont::drawText(canvas, 13 + content_x, 222, "A open    B back",
                             menu::FontFace::Regular14);
}

void PreferencesApp::applyRuntime(AppContext& context) {
    context.audio.setMasterVolume(context.settings.volume());
    context.audio.setEnabled(context.settings.audioEnabled());
    context.haptics.setEnabled(context.settings.hapticsEnabled());
}

void PreferencesApp::markChanged(AppContext& context) {
    applyRuntime(context);
    save_delay_ms_ = 800;
    std::snprintf(status_, sizeof(status_), "CHANGED - AUTO SAVE");
    context.invalidate();
}

void PreferencesApp::onEnter(AppContext& context) {
    selected_ = 0;
    save_delay_ms_ = 0;
    applyRuntime(context);
    std::snprintf(status_, sizeof(status_), context.settings.ok() ? "NVS READY" : "NVS ERROR");
    context.invalidate();
}

void PreferencesApp::onExit(AppContext& context) {
    if (context.settings.dirty()) {
        const esp_err_t err = context.settings.save();
        std::snprintf(status_, sizeof(status_), err == ESP_OK ? "SAVED" : "SAVE ERROR");
    }
}

void PreferencesApp::onEvent(AppContext& context, const input::Event& event) {
    if (!nav_event(event)) return;

    if (event.button == input::Button::Top) {
        selected_ = (selected_ + 4) % 5;
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate();
        return;
    }
    if (event.button == input::Button::Down) {
        selected_ = (selected_ + 1) % 5;
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate();
        return;
    }
    if (event.type == input::EventType::Pressed && event.button == input::Button::B) {
        if (context.settings.dirty()) context.settings.save();
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Back);
        context.launch("settings");
        return;
    }
    if (event.type == input::EventType::Pressed && event.button == input::Button::Start) {
        context.settings.resetDefaults(false);
        markChanged(context);
        context.haptics.play(haptics::Effect::Confirm);
        context.uiSound(audio::Effect::Confirm);
        return;
    }

    const bool left = event.button == input::Button::Left;
    const bool right = event.button == input::Button::Right;
    const bool activate = event.type == input::EventType::Pressed && event.button == input::Button::A;
    if (!left && !right && !activate) return;

    switch (selected_) {
        case 0: {
            const int delta = left ? -5 : 5;
            const int value = std::clamp<int>(static_cast<int>(context.settings.volume()) + delta, 0, 100);
            context.settings.setVolume(static_cast<uint8_t>(value));
            break;
        }
        case 1:
            context.settings.setAudioEnabled(!context.settings.audioEnabled());
            break;
        case 2:
            context.settings.setUiSoundsEnabled(!context.settings.uiSoundsEnabled());
            break;
        case 3:
            context.settings.setHapticsEnabled(!context.settings.hapticsEnabled());
            break;
        case 4: {
            int value = static_cast<int>(context.settings.motionSensitivity());
            value += left ? -1 : 1;
            if (value < 0) value = 2;
            if (value > 2) value = 0;
            context.settings.setMotionSensitivity(static_cast<uint8_t>(value));
            break;
        }
        default:
            return;
    }

    markChanged(context);
    context.haptics.play(haptics::Effect::Click);
    context.uiSound(audio::Effect::Click);
}

void PreferencesApp::update(AppContext& context, uint32_t dt_ms) {
    if (save_delay_ms_ == 0 || !context.settings.dirty()) return;
    if (dt_ms >= save_delay_ms_) {
        save_delay_ms_ = 0;
        const esp_err_t err = context.settings.save();
        std::snprintf(status_, sizeof(status_), err == ESP_OK ? "SAVED TO NVS" : "NVS SAVE ERROR");
        context.invalidate({20, 188, 360, 24});
    } else {
        save_delay_ms_ -= dt_ms;
    }
}

void PreferencesApp::draw(AppContext& context, const gfx::Rect&) {
    auto& c = context.gfx.canvas();
    c.clear_clip(gfx::WHITE);
    menu::PogoFont::drawText(c, 12, -1, "preferences", menu::FontFace::Italic22);
    draw_battery(c, context.power.state(), save_delay_ms_, 366, 8);

    const char* labels[] = {"master volume", "audio output", "ui sounds", "haptics", "motion sensitivity"};
    char values[5][20]{};
    std::snprintf(values[0], sizeof(values[0]), "%u%%", static_cast<unsigned>(context.settings.volume()));
    std::snprintf(values[1], sizeof(values[1]), "%s", context.settings.audioEnabled() ? "ON" : "OFF");
    std::snprintf(values[2], sizeof(values[2]), "%s", context.settings.uiSoundsEnabled() ? "ON" : "OFF");
    std::snprintf(values[3], sizeof(values[3]), "%s", context.settings.hapticsEnabled() ? "ON" : "OFF");
    std::snprintf(values[4], sizeof(values[4]), "%s", settings::motion_sensitivity_name(context.settings.motionSensitivity()));

    for (int i = 0; i < 5; ++i) {
        const int y = 41 + i * 31;
        const bool focus = i == selected_;
        if (focus) fill_pill(c, 9, y + 1, 382, 26, gfx::BLACK);
        const gfx::Color color = focus ? gfx::WHITE : gfx::BLACK;
        const menu::FontFace face = focus
            ? menu::FontFace::Italic14 : menu::FontFace::Regular14;
        menu::PogoFont::drawText(c, 17, y, labels[i], face, color);
        const int value_x = 381 - menu::PogoFont::textWidth(face, values[i]);
        menu::PogoFont::drawText(c, value_x, y, values[i], face, color);
    }

    menu::PogoFont::drawText(c, 14, 200, status_, menu::FontFace::Italic14);
    menu::PogoFont::drawText(c, 14, 220, "L/R or A change   B back   START defaults",
                             menu::FontFace::Regular14);
}

void MotionLabApp::onEnter(AppContext& context) {
    latest_ = context.imu.sample();
    last_sequence_ = latest_.sequence;
    zero_roll_ = 0;
    zero_pitch_ = 0;
    visual_roll_ = 0;
    visual_pitch_ = 0;
    visual_initialized_ = false;
    context.invalidate();
}
void MotionLabApp::onEvent(AppContext& context, const input::Event& event) {
    if (event.type != input::EventType::Pressed) return;
    if (event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click); context.uiSound(audio::Effect::Back); context.launch("settings");
    } else if (event.button == input::Button::A) {
        latest_ = context.imu.sample();
        zero_roll_ = latest_.roll;
        zero_pitch_ = latest_.pitch;
        visual_roll_ = 0;
        visual_pitch_ = 0;
        visual_initialized_ = true;
        context.haptics.play(haptics::Effect::Confirm);
        context.uiSound(audio::Effect::Confirm);
        context.invalidate();
    }
}
void MotionLabApp::update(AppContext& context, uint32_t dt_ms) {
    const imu::Sample sample = context.imu.sample();
    bool numbers_changed = false;
    if (sample.sequence != last_sequence_) {
        latest_ = sample;
        last_sequence_ = sample.sequence;
        numbers_changed = true;
    }

    // Visual-only filtering: keep the numeric readout immediate, but make the
    // spirit-level display calm and readable. The signs are intentionally
    // inverted so the ball follows the physical direction of the tilt.
    const float sensitivity_values[] = {0.65f, 1.0f, 1.45f};
    const uint8_t sensitivity_index = std::min<uint8_t>(context.settings.motionSensitivity(), 2);
    const float sensitivity = sensitivity_values[sensitivity_index];
    const float target_roll = std::clamp(-(latest_.roll - zero_roll_) * sensitivity, -45.0f, 45.0f);
    const float target_pitch = std::clamp(-(latest_.pitch - zero_pitch_) * sensitivity, -45.0f, 45.0f);

    if (!visual_initialized_) {
        visual_roll_ = target_roll;
        visual_pitch_ = target_pitch;
        visual_initialized_ = true;
    } else {
        const float dt = std::clamp(static_cast<float>(dt_ms), 1.0f, 50.0f);
        const float alpha = 1.0f - std::exp(-dt / 170.0f);
        visual_roll_ += (target_roll - visual_roll_) * alpha;
        visual_pitch_ += (target_pitch - visual_pitch_) * alpha;
    }

    const bool visual_moving = std::fabs(target_roll - visual_roll_) > 0.03f ||
                               std::fabs(target_pitch - visual_pitch_) > 0.03f;
    if (numbers_changed) context.invalidate({15, 37, 370, 177});
    else if (visual_moving) context.invalidate({214, 50, 154, 128});
}
void MotionLabApp::draw(AppContext& context, const gfx::Rect&) {
    auto& c = context.gfx.canvas(); c.clear_clip(context.theme.background);
    char right[24]; std::snprintf(right, sizeof(right), "ID %02X", context.imu.chipId());
    gui::draw_header(c, context.theme, "MOTION LAB", context.imu.ok() ? right : "IMU ERROR");
    gui::Panel panel({16, 41, 368, 166}); panel.draw(c, context.theme);
    char line[64];
    std::snprintf(line, sizeof(line), "ACC  X %+1.2f", latest_.ax); c.draw_text(27, 55, line, gfx::font5x7(), context.theme.foreground);
    std::snprintf(line, sizeof(line), "     Y %+1.2f", latest_.ay); c.draw_text(27, 70, line, gfx::font5x7(), context.theme.foreground);
    std::snprintf(line, sizeof(line), "     Z %+1.2f g", latest_.az); c.draw_text(27, 85, line, gfx::font5x7(), context.theme.foreground);
    std::snprintf(line, sizeof(line), "GYR  X %+4.0f", latest_.gx); c.draw_text(27, 108, line, gfx::font5x7(), context.theme.foreground);
    std::snprintf(line, sizeof(line), "     Y %+4.0f", latest_.gy); c.draw_text(27, 123, line, gfx::font5x7(), context.theme.foreground);
    std::snprintf(line, sizeof(line), "     Z %+4.0f dps", latest_.gz); c.draw_text(27, 138, line, gfx::font5x7(), context.theme.foreground);
    const float roll = latest_.roll - zero_roll_, pitch = latest_.pitch - zero_pitch_;
    std::snprintf(line, sizeof(line), "R%+3.0f  P%+3.0f  ERR%lu", roll, pitch,
                  static_cast<unsigned long>(latest_.read_errors));
    c.draw_text(27, 176, line, gfx::font5x7(), context.theme.foreground);

    const gfx::Rect box{220, 57, 142, 112};
    c.draw_rect(box.x, box.y, box.w, box.h, context.theme.border);
    const int cx = box.x + box.w / 2;
    const int cy = box.y + box.h / 2;

    // Fixed reference marks make the moving horizon much easier to read.
    for (int x = box.x + 10; x < box.right() - 10; x += 12)
        c.draw_line(x, cy, std::min(x + 5, box.right() - 10), cy, context.theme.foreground);
    c.draw_circle(cx, cy, 20, context.theme.foreground);
    c.draw_circle(cx, cy, 38, context.theme.foreground);

    // Smooth artificial horizon. Its angle and vertical travel are deliberately
    // limited so it no longer snaps into the frame edges.
    const float horizon_degrees = std::clamp(visual_roll_, -28.0f, 28.0f);
    const float a = horizon_degrees * 0.01745329252f;
    const int horizon_y = cy + static_cast<int>(std::lround(visual_pitch_ * 0.34f));
    const int half = 52;
    const int dx = static_cast<int>(std::lround(std::cos(a) * half));
    const int dy = static_cast<int>(std::lround(std::sin(a) * half));
    c.draw_line(cx - dx, horizon_y - dy, cx + dx, horizon_y + dy, context.theme.foreground);

    // The ball follows the physical tilt direction, with gentler travel and
    // rounded coordinates so even small movements around the centre are visible.
    const int bx = std::clamp(cx + static_cast<int>(std::lround(visual_pitch_ * 0.90f)),
                              box.x + 9, box.right() - 9);
    const int by = std::clamp(cy + static_cast<int>(std::lround(visual_roll_ * 0.65f)),
                              box.y + 9, box.bottom() - 9);
    c.fill_circle(bx, by, 5, context.theme.foreground);
    c.draw_line(cx - 5, cy, cx + 5, cy, context.theme.foreground);
    c.draw_line(cx, cy - 5, cx, cy + 5, context.theme.foreground);
    char motion_footer[32];
    std::snprintf(motion_footer, sizeof(motion_footer), "BMI270 %s", settings::motion_sensitivity_name(context.settings.motionSensitivity()));
    gui::draw_footer(c, context.theme, "A ZERO  B BACK", motion_footer);
}

void PowerStatusApp::onEnter(AppContext& context) {
    context.power.forceRefresh(); latest_ = context.power.state(); last_sequence_ = 0; context.invalidate();
}
void PowerStatusApp::onEvent(AppContext& context, const input::Event& event) {
    if (event.type == input::EventType::Pressed && event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click); context.uiSound(audio::Effect::Back); context.launch("settings");
    }
}
void PowerStatusApp::update(AppContext& context, uint32_t) {
    const power::State state = context.power.state();
    if (state.sequence != last_sequence_) { latest_ = state; last_sequence_ = state.sequence; context.invalidate({15,37,370,178}); }
}
void PowerStatusApp::draw(AppContext& context, const gfx::Rect&) {
    auto& c = context.gfx.canvas(); c.clear_clip(context.theme.background);
    gui::draw_header(c, context.theme, "POWER STATUS", latest_.ok ? "BQ24295 OK" : "BQ ERROR");
    gui::Panel panel({17, 42, 366, 164}); panel.draw(c, context.theme);
    char line[96];
    std::snprintf(line, sizeof(line), "BATTERY  %s  %umV", latest_.battery_valid ? "VALID" : "WAIT",
                  static_cast<unsigned>(latest_.battery_mv));
    c.draw_text(30, 57, line, gfx::font5x7(), context.theme.foreground);
    gui::ProgressBar battery({30, 76, 340, 18}); battery.setRange(0,100); battery.setValue(latest_.battery_percent); battery.setShowValue(true); battery.draw(c, context.theme);
    std::snprintf(line, sizeof(line), "USB: %-3s    CHARGE: %-3s", latest_.usb_present ? "YES" : "NO", latest_.charging ? "YES" : "NO");
    c.draw_text(30, 108, line, gfx::font5x7(), context.theme.foreground);
    std::snprintf(line, sizeof(line), "REG08 %02X  REG09 %02X  I2C ERR %lu", latest_.reg08, latest_.reg09,
                  static_cast<unsigned long>(latest_.i2c_errors));
    c.draw_text(30, 127, line, gfx::font5x7(), context.theme.foreground);
    c.draw_text(30, 151, "HOLD POWER BUTTON 2 SEC", gfx::font5x7(), context.theme.foreground);
    gui::ProgressBar hold({30, 168, 340, 16}); hold.setRange(0,100); hold.setValue(latest_.hold_percent); hold.draw(c, context.theme);
    c.draw_text(30, 190, latest_.usb_present ? "USB BLOCKS SHIP MODE" : "RELEASE, THEN BATFET OFF", gfx::font5x7(), context.theme.foreground);
    draw_back_footer(context);
}

void AboutApp::onEnter(AppContext& context) {
    context.invalidate();
}

void AboutApp::onEvent(AppContext& context, const input::Event& event) {
    if (event.type == input::EventType::Pressed && event.button == input::Button::B) {
        context.haptics.play(haptics::Effect::Click);
        context.uiSound(audio::Effect::Back);
        context.launch("settings");
    }
}

void AboutApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(gfx::WHITE);
    menu::PogoFont::drawText(canvas, 12, -1, "about pogopo", menu::FontFace::Italic22);
    draw_battery(canvas, context.power.state(), 0, 366, 8);
    canvas.draw_rect(12, 40, 376, 164, gfx::BLACK);
    menu::PogoFont::drawText(canvas, 28, 50, "pogopoOS 2.0  /  STEP13.4.2",
                             menu::FontFace::Italic14);
    menu::PogoFont::drawText(canvas, 28, 78,
        "ESP32-S3  16 MB flash  8 MB PSRAM\n"
        "Sharp Memory LCD  400 x 240\n"
        "Peanut-GB  /  32768 Hz I2S\n"
        "BMI270  TCA9555  BQ24295  SDMMC",
        menu::FontFace::Regular14);
    menu::PogoFont::drawText(canvas, 28, 172,
        "animated ui revision 1", menu::FontFace::Italic14);
    menu::PogoFont::drawText(canvas, 14, 220, "B back",
                             menu::FontFace::Regular14);
}

} // namespace pogopo::demo
