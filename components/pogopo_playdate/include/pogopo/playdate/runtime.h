#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "pogopo/gfx/canvas.h"
#include "pogopo_audio.h"
#include "pogopo_storage.h"

namespace pogopo::playdate {

struct Stats {
    uint32_t lua_frames = 0;
    uint32_t last_update_us = 0;
    uint32_t max_update_us = 0;
    uint32_t errors = 0;
    uint32_t requested_fps = 50;
    size_t lua_bytes = 0;
    size_t lua_peak_bytes = 0;
    size_t lua_gc_bytes = 0;
};

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    esp_err_t start(gfx::Canvas& canvas, audio::Audio& audio,
                    storage::Storage& storage);
    void stop();

    void setInput(uint8_t held_mask, uint8_t pressed_mask);
    // Advances the native Lua VM. Returns the number of Playdate frames run.
    uint32_t update(uint32_t dt_ms);

    bool running() const;
    const char* error() const;
    Stats stats() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace pogopo::playdate
