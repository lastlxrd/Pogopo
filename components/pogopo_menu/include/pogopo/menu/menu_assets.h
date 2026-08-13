#pragma once

#include <cstddef>
#include <cstdint>

#include "pogopo/gfx/bitmap.h"

namespace pogopo::menu {

enum class Art : uint8_t {
    Pogopo = 0,
    GameBoy,
    Playdate,
    GameBoyFull,
    Count,
};

struct AnimationInfo {
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t frame_count = 0;
    int16_t source_x = 0;
    int16_t source_y = 0;
};

class Assets {
public:
    static bool valid();
    static size_t embeddedSize();
    static const uint8_t* base();

    static AnimationInfo info(Art art);
    static gfx::Bitmap frame(Art art, size_t frame_index);
    static size_t frameAtTime(Art art, uint32_t elapsed_ms);
    static uint16_t frameDuration(Art art, size_t frame_index);
};

} // namespace pogopo::menu
