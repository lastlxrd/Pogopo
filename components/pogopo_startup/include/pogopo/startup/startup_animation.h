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
    static constexpr size_t FRAME_COUNT = 122;
    static constexpr size_t LOOP_FRAME_COUNT = 5;
    static constexpr size_t LOOP_START = FRAME_COUNT - LOOP_FRAME_COUNT;
    static constexpr uint32_t FRAME_PERIOD_MS = 100;

    bool valid() const;
    size_t embeddedSize() const;
    esp_err_t show(gfx::Graphics& graphics, size_t frame_index) const;
};

} // namespace pogopo::startup
