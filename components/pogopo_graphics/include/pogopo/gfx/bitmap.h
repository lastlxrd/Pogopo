#pragma once

#include <cstdint>
#include "pogopo/gfx/types.h"

namespace pogopo::gfx {

struct Bitmap {
    int width = 0;
    int height = 0;
    int stride = 0;
    const uint8_t* data = nullptr;
    BitOrder bit_order = BitOrder::MSB_FIRST;

    constexpr bool valid() const {
        return data != nullptr && width > 0 && height > 0 && stride > 0;
    }

    bool pixel(int x, int y) const {
        if (!valid() || x < 0 || y < 0 || x >= width || y >= height) {
            return false;
        }
        const uint8_t value = data[y * stride + (x >> 3)];
        const uint8_t mask = bit_order == BitOrder::MSB_FIRST
            ? static_cast<uint8_t>(0x80U >> (x & 7))
            : static_cast<uint8_t>(1U << (x & 7));
        return (value & mask) != 0;
    }
};

inline Bitmap make_bitmap_1bpp(int width, int height, const uint8_t* data,
                               BitOrder order = BitOrder::MSB_FIRST,
                               int stride = 0) {
    if (stride <= 0) {
        stride = (width + 7) / 8;
    }
    return {width, height, stride, data, order};
}

} // namespace pogopo::gfx
