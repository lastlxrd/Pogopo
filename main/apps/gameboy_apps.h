#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "pogopo_app.h"
#include "pogopo_gui.h"
#include "pogopo_gameboy.h"

namespace pogopo::demo {

class GameBoyApp final : public Application {
public:
    explicit GameBoyApp(gameboy::GameBoy& emulator) : emulator_(emulator) {}

    const char* id() const override { return "gameboy"; }
    const char* title() const override { return "Game Boy"; }
    bool capturesMenuButton() const override { return true; }

    void prepare(const char* path, const char* display_name, gameboy::ScaleMode scale);
    void onEnter(AppContext& context) override;
    void onExit(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void update(AppContext& context, uint32_t dt_ms) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;

private:
    void drawLoading(AppContext& context);
    void updateButtons(AppContext& context);

    gameboy::GameBoy& emulator_;
    char rom_path_[192]{};
    char display_name_[64]{};
    gameboy::ScaleMode scale_ = gameboy::ScaleMode::OneX;
    esp_err_t load_error_ = ESP_ERR_INVALID_STATE;
    uint32_t last_sequence_ = UINT32_MAX;
    uint32_t exit_hold_ms_ = 0;
    bool force_draw_ = true;
    uint32_t perf_elapsed_ms_ = 0;
    gameboy::Stats perf_previous_{};
};

class GameBoyBrowserApp final : public Application {
public:
    explicit GameBoyBrowserApp(GameBoyApp& player) : player_(player) {}

    const char* id() const override { return "gb_browser"; }
    const char* title() const override { return "Game Boy"; }

    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;

private:
    void rescan(AppContext& context);
    void launchSelected(AppContext& context);

    GameBoyApp& player_;
    gui::List list_{{18, 42, 364, 148}};
    std::array<storage::FileEntry, storage::Storage::MAX_FILES> files_{};
    std::array<gui::ListItem, storage::Storage::MAX_FILES> items_{};
    std::array<std::array<char, 32>, storage::Storage::MAX_FILES> subtitles_{};
    size_t file_count_ = 0;
    gameboy::ScaleMode scale_ = gameboy::ScaleMode::OneX;
    char status_[80] = "READY";
};

} // namespace pogopo::demo
