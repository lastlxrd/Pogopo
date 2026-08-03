#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "pogopo_app.h"
#include "pogopo_gameboy_advance.h"
#include "pogopo_gui.h"

namespace pogopo::demo {

class GameBoyAdvanceApp final : public Application {
public:
    explicit GameBoyAdvanceApp(gameboy_advance::GameBoyAdvance& emulator)
        : emulator_(emulator) {}

    const char* id() const override { return "gameboy_advance"; }
    const char* title() const override { return "Game Boy Advance"; }
    bool capturesMenuButton() const override { return true; }

    void prepare(const char* path, const char* display_name,
                 gameboy_advance::ScaleMode scale);
    void onEnter(AppContext& context) override;
    void onExit(AppContext& context) override;
    void onSuspend(AppContext& context) override;
    void onResume(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void update(AppContext& context, uint32_t dt_ms) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;

private:
    void drawLoading(AppContext& context);
    void updateButtons(AppContext& context);

    gameboy_advance::GameBoyAdvance& emulator_;
    char rom_path_[192]{};
    char display_name_[64]{};
    gameboy_advance::ScaleMode scale_ = gameboy_advance::ScaleMode::OneX;
    esp_err_t load_error_ = ESP_ERR_INVALID_STATE;
    uint32_t last_sequence_ = UINT32_MAX;
    uint32_t exit_hold_ms_ = 0;
    bool force_draw_ = true;
    uint32_t perf_elapsed_ms_ = 0;
    gameboy_advance::Stats perf_previous_{};
};

class GameBoyAdvanceBrowserApp final : public Application {
public:
    explicit GameBoyAdvanceBrowserApp(GameBoyAdvanceApp& player) : player_(player) {}

    const char* id() const override { return "gba_browser"; }
    const char* title() const override { return "Game Boy Advance"; }

    void onEnter(AppContext& context) override;
    void onEvent(AppContext& context, const input::Event& event) override;
    void draw(AppContext& context, const gfx::Rect& dirty_region) override;

private:
    void rescan(AppContext& context);
    void launchSelected(AppContext& context);

    GameBoyAdvanceApp& player_;
    gui::List list_{{18, 42, 364, 148}};
    std::array<storage::FileEntry, storage::Storage::MAX_FILES> files_{};
    std::array<gui::ListItem, storage::Storage::MAX_FILES> items_{};
    std::array<std::array<char, 32>, storage::Storage::MAX_FILES> subtitles_{};
    size_t file_count_ = 0;
    gameboy_advance::ScaleMode scale_ = gameboy_advance::ScaleMode::OneX;
    char status_[80] = "EXPERIMENTAL";
};

} // namespace pogopo::demo
