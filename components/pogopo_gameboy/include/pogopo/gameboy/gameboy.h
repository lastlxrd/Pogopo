#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pogopo_audio.h"
#include "pogopo/gfx/gfx.h"

namespace pogopo::gameboy {

struct Buttons {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool a = false;
    bool b = false;
    bool start = false;
    bool select = false;
};

enum class ScaleMode : uint8_t {
    OneX,
    FitHeight,
};

struct Stats {
    uint32_t emulated_frames = 0;
    uint32_t displayed_frames = 0;
    uint32_t audio_frames_pushed = 0;
    uint32_t audio_frames_dropped = 0;
    uint32_t cache_hits = 0;
    uint32_t cache_misses = 0;
    uint32_t cache_fill_us = 0;
    uint32_t save_writes = 0;
    uint32_t last_frame_us = 0;
    uint32_t max_frame_us = 0;
    uint32_t rom_bytes = 0;
    uint32_t save_bytes = 0;
    uint32_t rom_arena_bytes = 0;
    uint32_t frame_buffer_bytes = 0;
    uint8_t cache_pages = 0;
    bool rom_in_arena = false;
    bool rom_in_psram = false;
};

class GameBoy {
public:
    static constexpr int SCREEN_WIDTH = 160;
    static constexpr int SCREEN_HEIGHT = 144;
    static constexpr size_t FRAME_PIXELS = SCREEN_WIDTH * SCREEN_HEIGHT;
    static constexpr size_t FRAME_BYTES = (FRAME_PIXELS + 3U) / 4U;

    struct Config {
        // Reserve as much of this fast-RAM budget as is safe before the
        // background tasks fragment the heap. Small ROMs run directly from
        // it; PSRAM ROMs reuse it as a 16 KiB bank cache.
        uint32_t internal_rom_arena_bytes = 128U * 1024U;
        uint32_t internal_rom_arena_min_bytes = 32U * 1024U;
        uint32_t internal_rom_headroom_bytes = 128U * 1024U;
        uint32_t internal_rom_limit = 256U * 1024U;
        uint32_t save_flush_interval_ms = 0;
        uint8_t realtime_volume = 74;
        // The stable Arduino build used up to eight 16 KiB internal cache
        // pages for cartridges that had to remain in PSRAM.
        uint8_t requested_cache_pages = 8;
        // Keep Peanut-GB LCD rendering enabled every emulated frame for
        // compatibility. The frontend publishes only every Nth frame.
        bool peanut_frame_skip = true;
        uint8_t display_divider = 1;
        bool dither = true;
        UBaseType_t task_priority = 6;
        BaseType_t task_core = 1;
        uint32_t task_stack = 8192;
        // Match the stable Arduino split: MiniGB APU generation is paced by
        // the fixed-rate I2S consumer on Core 0, not by emulator frame time.
        UBaseType_t audio_task_priority = 7;
        BaseType_t audio_task_core = 0;
        uint32_t audio_task_stack = 4096;
    };

    GameBoy() = default;
    ~GameBoy();
    GameBoy(const GameBoy&) = delete;
    GameBoy& operator=(const GameBoy&) = delete;

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

    // The arena is reserved at boot for the stable Game Boy ROM cache. The
    // app manager never runs GB and GBA games concurrently, so the isolated
    // GBA frontend may use this otherwise-idle storage while no GB ROM is
    // loaded. The Game Boy remains the owner and the pointer must not be
    // freed or retained after a GBA game exits.
    uint8_t* idleInternalArena() const;
    uint32_t idleInternalArenaSize() const;

    // Used by Peanut-GB's global sound callbacks in the implementation TU.
    uint8_t apuRead(uint16_t address);
    void apuWrite(uint16_t address, uint8_t value);

private:
    struct Impl;

    static void taskEntry(void* argument);
    static void audioTaskEntry(void* argument);
    void taskLoop();
    void audioTaskLoop();
    esp_err_t allocateFrames();
    void freeFrames();
    esp_err_t loadRomFile(const char* path);
    esp_err_t initializeCore();
    esp_err_t initializeSaveRam();
    void freeRomAndSave();
    bool initializeRomCache();
    uint8_t readRom(uint32_t address);
    void pushAudioFrame();
    void setCoreButtons(uint8_t mask);

    Config config_{};
    audio::Audio* audio_ = nullptr;
    Impl* impl_ = nullptr;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t audio_task_ = nullptr;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> loaded_{false};
    std::atomic<bool> task_running_{false};
    std::atomic<bool> audio_task_running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<uint8_t> button_mask_{0};
    std::atomic<uint32_t> frame_sequence_{0};
    std::atomic<esp_err_t> last_error_{ESP_OK};
};

const char* scale_mode_name(ScaleMode mode);

} // namespace pogopo::gameboy

namespace pogopo {
using GameBoy = gameboy::GameBoy;
using GameBoyButtons = gameboy::Buttons;
using GameBoyScaleMode = gameboy::ScaleMode;
}
