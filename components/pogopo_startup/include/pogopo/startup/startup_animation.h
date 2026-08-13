#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace pogopo::gfx {
class Graphics;
}

namespace pogopo::startup {

class StartupAnimation {
public:
    static constexpr int WIDTH = 400;
    static constexpr int HEIGHT = 240;
    static constexpr int STRIDE = WIDTH / 8;
    static constexpr size_t FRAME_SIZE = STRIDE * HEIGHT;
    static constexpr size_t FRAME_COUNT = 25;
    // Human frames 12..15 are the interactive idle loop. After a fresh button
    // press, playback resumes at human frame 16 and runs through frame 25.
    static constexpr size_t LOOP_START = 11;
    static constexpr size_t LOOP_END = 15; // Exclusive, so the last loop frame is 15.
    static constexpr size_t RESUME_START = LOOP_END;
    static constexpr uint32_t FRAME_PERIOD_MS = 100;

    bool valid() const;
    size_t embeddedSize() const;
    esp_err_t show(gfx::Graphics& graphics, size_t frame_index) const;
};

class OutroAnimation {
public:
    static constexpr int WIDTH = StartupAnimation::WIDTH;
    static constexpr int HEIGHT = StartupAnimation::HEIGHT;
    static constexpr int STRIDE = StartupAnimation::STRIDE;
    static constexpr size_t FRAME_SIZE = StartupAnimation::FRAME_SIZE;
    static constexpr size_t FRAME_DATA_SIZE = FRAME_SIZE * 2;
    // Source frame 1 was an unwanted full-white flash. Packed frame 1 is the
    // supplied Aseprite frame 2; packed frame 12 is supplied frame 13.
    static constexpr size_t FRAME_COUNT = 24;
    static constexpr size_t SLOW_LAST_FRAME = 11; // Source human frame 13.
    static constexpr uint32_t SLOW_FRAME_PERIOD_MS = 100; // 10 FPS.
    static constexpr uint32_t FAST_FRAME_PERIOD_MS = 67;  // About 15 FPS.

    bool valid() const;
    size_t embeddedSize() const;
    esp_err_t show(gfx::Graphics& graphics,
                   size_t frame_index,
                   const uint8_t* underlay,
                   size_t underlay_size) const;
};

} // namespace pogopo::startup
