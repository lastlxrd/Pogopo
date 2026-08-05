#include "apps/demo_apps.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace pogopo::demo {

namespace {
constexpr gui::ListItem kLauncherItems[] = {
    {"Game Boy", "Peanut-GB emulator + SD ROMs", "gb_browser", true},
    {"Playdate games (SD)", "Open extracted .pdx packages", "pogodate_browser", true},
    {"PogoDate: Celeste", "Celeste Classic 1.0.3 Lua", "pogodate_celeste", true},
    {"PogoDate: PDSnake", "Original Playdate Lua source", "pogodate", true},
    {"Graphics demo", "Sprites + partial redraw", "graphics", true},
    {"Input monitor", "Buttons and event state", "input", true},
    {"Audio lab", "I2S mixer + generated sounds", "audio", true},
    {"WAV player", "Streaming PCM from SD", "wav", true},
    {"Motion lab", "BMI270 accel + gyro", "motion", true},
    {"Power status", "Battery, USB and shutdown", "power", true},
    {"Haptics lab", "Play vibration patterns", "haptics", true},
    {"Settings", "Persistent NVS options", "settings", true},
    {"About Pogopo", "Framework information", "about", true},
};

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

LauncherApp::LauncherApp() {
    list_.setItems(kLauncherItems, sizeof(kLauncherItems) / sizeof(kLauncherItems[0]));
    list_.setRowHeight(32);
}

void LauncherApp::onEnter(AppContext& context) {
    context.invalidate();
}

void LauncherApp::onEvent(AppContext& context, const input::Event& event) {
    if (!nav_event(event)) return;
    if (event.button == input::Button::Top && list_.move(-1)) {
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate(list_.bounds());
    } else if (event.button == input::Button::Down && list_.move(1)) {
        context.haptics.play(haptics::Effect::Tick);
        context.uiSound(audio::Effect::Tick);
        context.invalidate(list_.bounds());
    } else if (event.type == input::EventType::Pressed && event.button == input::Button::A) {
        const gui::ListItem* item = list_.selectedItem();
        if (item && item->id) {
            context.haptics.play(haptics::Effect::Confirm);
            context.uiSound(audio::Effect::Confirm);
            context.launch(item->id);
        }
    }
}

void LauncherApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(context.theme.background);
    gui::draw_header(canvas, context.theme, "POGOPO OS 2.0", "STEP11.2");
    list_.draw(canvas, context.theme);
    gui::draw_footer(canvas, context.theme, "UP/DOWN MOVE   A OPEN", "MENU SYSTEM");
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
        context.home();
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
        context.home();
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
        context.home();
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
        context.home();
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
        context.home(); // Music intentionally keeps playing in the background.
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

void SettingsApp::applyRuntime(AppContext& context) {
    context.audio.setMasterVolume(context.settings.volume());
    context.audio.setEnabled(context.settings.audioEnabled());
    context.haptics.setEnabled(context.settings.hapticsEnabled());
}

void SettingsApp::markChanged(AppContext& context) {
    applyRuntime(context);
    save_delay_ms_ = 800;
    std::snprintf(status_, sizeof(status_), "CHANGED - AUTO SAVE");
    context.invalidate();
}

void SettingsApp::onEnter(AppContext& context) {
    selected_ = 0;
    save_delay_ms_ = 0;
    applyRuntime(context);
    std::snprintf(status_, sizeof(status_), context.settings.ok() ? "NVS READY" : "NVS ERROR");
    context.invalidate();
}

void SettingsApp::onExit(AppContext& context) {
    if (context.settings.dirty()) {
        const esp_err_t err = context.settings.save();
        std::snprintf(status_, sizeof(status_), err == ESP_OK ? "SAVED" : "SAVE ERROR");
    }
}

void SettingsApp::onEvent(AppContext& context, const input::Event& event) {
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
        context.home();
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

void SettingsApp::update(AppContext& context, uint32_t dt_ms) {
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

void SettingsApp::draw(AppContext& context, const gfx::Rect&) {
    auto& c = context.gfx.canvas();
    c.clear_clip(context.theme.background);
    gui::draw_header(c, context.theme, "SYSTEM SETTINGS", context.settings.dirty() ? "UNSAVED" : "NVS");

    const char* labels[] = {"MASTER VOLUME", "AUDIO OUTPUT", "UI SOUNDS", "HAPTICS", "MOTION SENS."};
    char values[5][20]{};
    std::snprintf(values[0], sizeof(values[0]), "%u%%", static_cast<unsigned>(context.settings.volume()));
    std::snprintf(values[1], sizeof(values[1]), "%s", context.settings.audioEnabled() ? "ON" : "OFF");
    std::snprintf(values[2], sizeof(values[2]), "%s", context.settings.uiSoundsEnabled() ? "ON" : "OFF");
    std::snprintf(values[3], sizeof(values[3]), "%s", context.settings.hapticsEnabled() ? "ON" : "OFF");
    std::snprintf(values[4], sizeof(values[4]), "%s", settings::motion_sensitivity_name(context.settings.motionSensitivity()));

    for (int i = 0; i < 5; ++i) {
        const int y = 43 + i * 28;
        const bool focus = i == selected_;
        const gfx::Color bg = focus ? context.theme.focus_background : context.theme.background;
        const gfx::Color fg = focus ? context.theme.focus_foreground : context.theme.foreground;
        c.fill_rect(20, y, 360, 25, bg);
        c.draw_rect(20, y, 360, 25, context.theme.border);
        c.draw_text(30, y + 9, labels[i], gfx::font5x7(), fg, 1, true, bg);
        const int value_x = 366 - static_cast<int>(std::strlen(values[i])) * 6;
        c.draw_text(value_x, y + 9, values[i], gfx::font5x7(), fg, 1, true, bg);
    }

    c.draw_text(23, 190, status_, gfx::font5x7(), context.theme.foreground);
    gui::draw_footer(c, context.theme, "L/R OR A CHANGE  B BACK", "START DEFAULTS");
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
        context.haptics.play(haptics::Effect::Click); context.uiSound(audio::Effect::Back); context.home();
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
        context.haptics.play(haptics::Effect::Click); context.uiSound(audio::Effect::Back); context.home();
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
        context.home();
    }
}

void AboutApp::draw(AppContext& context, const gfx::Rect&) {
    auto& canvas = context.gfx.canvas();
    canvas.clear_clip(context.theme.background);
    gui::draw_header(canvas, context.theme, "ABOUT POGOPO", "ESP-IDF");

    gui::Panel panel({18, 42, 364, 165});
    panel.draw(canvas, context.theme);
    canvas.draw_text(35, 56, "pogopoOS 2.0", gfx::font5x7(), context.theme.foreground, 2);
    gui::draw_wrapped_text(canvas, {35, 88, 330, 92},
        "Native ESP-IDF platform with graphics, GUI, input, haptics, mixed I2S audio, streaming SD WAV, persistent NVS settings, BMI270 motion and BQ24295 power management.",
        context.theme, 1, 3);
    canvas.draw_text(35, 183, "Sharp 400x240 / ESP32-S3 / STEP8", gfx::font5x7(), context.theme.foreground);
    draw_back_footer(context);
}

} // namespace pogopo::demo
