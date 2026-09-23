#pragma once

#include <cstddef>
#include <cstdint>

namespace pogopo::playdate {

struct CelesteAsset {
    const char* name;
    uint16_t sheet_width;
    uint16_t sheet_height;
    uint16_t frame_width;
    uint16_t frame_height;
    uint16_t frame_count;
    const uint8_t* packed_pixels;
};

const CelesteAsset* findCelesteAsset(const char* path);
uint8_t celesteAssetPixel(const CelesteAsset& asset, int x, int y);

} // namespace pogopo::playdate
