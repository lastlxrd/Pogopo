#pragma once

#include <algorithm>
#include <cstdint>

namespace pogopo::gfx {

enum class Color : uint8_t {
    BLACK = 0,
    WHITE = 1,
};

inline constexpr Color BLACK = Color::BLACK;
inline constexpr Color WHITE = Color::WHITE;

struct Point {
    int x = 0;
    int y = 0;
};

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    constexpr bool empty() const { return w <= 0 || h <= 0; }
    constexpr int right() const { return x + w; }
    constexpr int bottom() const { return y + h; }
    constexpr bool contains(int px, int py) const {
        return px >= x && py >= y && px < right() && py < bottom();
    }

    static Rect intersect(const Rect& a, const Rect& b) {
        const int nx = std::max(a.x, b.x);
        const int ny = std::max(a.y, b.y);
        const int nr = std::min(a.right(), b.right());
        const int nb = std::min(a.bottom(), b.bottom());
        return {nx, ny, std::max(0, nr - nx), std::max(0, nb - ny)};
    }
};

enum class BitOrder : uint8_t {
    MSB_FIRST,
    LSB_FIRST,
};

} // namespace pogopo::gfx

