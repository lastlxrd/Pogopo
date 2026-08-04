#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pogopo_audio.h"
#include "pogopo/gfx/gfx.h"

namespace pogopo::gameboy_advance {

struct Buttons {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool a = false;
    bool b = false;
    bool start = false;
    bool select = false;
    bool l = false;
    bool r = false;
};

enum class ScaleMode : uint8_t {
    OneX,
    FitHeight,
};

struct Stats {
    uint32_t emulated_frames = 0;
    uint32_t rendered_frames = 0;
    uint32_t displayed_frames = 0;
    uint32_t audio_frames_pushed = 0;
    uint32_t audio_frames_dropped = 0;
    uint32_t page_loads = 0;
    uint32_t page_load_us = 0;
    uint32_t code_cache_hits = 0;
    uint32_t code_cache_misses = 0;
    uint32_t code_cache_fill_us = 0;
    uint32_t save_writes = 0;
    uint32_t last_frame_us = 0;
    uint32_t max_frame_us = 0;
    uint32_t rom_bytes = 0;
    uint32_t rom_buffer_bytes = 0;
    uint32_t frame_buffer_bytes = 0;
    uint32_t save_bytes = 0;
    uint32_t fast_memory_bytes = 0;
    uint8_t code_cache_pages = 0;
    bool iwram_internal = false;
};

class GameBoyAdvance {
public:
    static constexpr int SCREEN_WIDTH = 240;
    static constexpr int SCREEN_HEIGHT = 160;
    static constexpr size_t FRAME_PIXELS = SCREEN_WIDTH * SCREEN_HEIGHT;
    static constexpr size_t FRAME_BYTES = FRAME_PIXELS * sizeof(uint16_t);

    struct Config {
        uint8_t realtime_volume = 72;
        uint8_t rom_buffer_megabytes = 6;
        // Optional storage borrowed from the idle stable-GB ROM arena. It is
        // used only between GBA load/unload and remains owned by GameBoy.
        uint8_t* fast_memory = nullptr;
        uint32_t fast_memory_bytes = 0;
        bool render_every_other_frame = true;
        bool dither = true;
        UBaseType_t task_priority = 6;
        BaseType_t task_core = 1;
        uint32_t task_stack = 16384;
    };

    GameBoyAdvance() = default;
    ~GameBoyAdvance();
    GameBoyAdvance(const GameBoyAdvance&) = delete;
    GameBoyAdvance& operator=(const GameBoyAdvance&) = delete;

    esp_err_t begin(audio::Audio& audio);
    esp_err_t begin(audio::Audio& audio, const Config& config);
    void end();

    esp_err_t load(const char* path);
    void unload();
    esp_err_t flushSave();
    void reset();
    void setPaused(bool paused);
    void setButtons(const Buttons& buttons);

    bool drawLatest(gfx::Canvas& canvas, ScaleMode mode, bool force_full = false);

    bool ready() const { return initialized_.load(); }
    bool loaded() const { return loaded_.load(); }
    bool running() const { return task_running_.load(); }
    bool paused() const { return paused_.load(); }
    uint32_t frameSequence() const { return frame_sequence_.load(); }
    const char* romTitle() const;
    const char* romPath() const;
    esp_err_t lastError() const { return last_error_.load(); }
    Stats stats() const;

    uint16_t coreButtonMask() const { return button_mask_.load(std::memory_order_acquire); }

private:
    struct Impl;

    static void taskEntry(void* argument);
    void taskLoop();
    esp_err_t allocateCoreMemory();
    void freeCoreMemory();
    esp_err_t loadSave();
    esp_err_t startAudio();
    void pushAudioForWallTime(uint32_t source_frames, int64_t now_us);
    void stopTask();
    uint32_t activeSaveSize() const;

    Config config_{};
    audio::Audio* audio_ = nullptr;
    Impl* impl_ = nullptr;
    TaskHandle_t task_ = nullptr;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> loaded_{false};
    std::atomic<bool> task_running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<uint16_t> button_mask_{0};
    std::atomic<uint32_t> frame_sequence_{0};
    std::atomic<esp_err_t> last_error_{ESP_OK};
};

const char* scale_mode_name(ScaleMode mode);

} // namespace pogopo::gameboy_advance

namespace pogopo {
using GameBoyAdvance = gameboy_advance::GameBoyAdvance;
using GameBoyAdvanceButtons = gameboy_advance::Buttons;
using GameBoyAdvanceScaleMode = gameboy_advance::ScaleMode;
}
