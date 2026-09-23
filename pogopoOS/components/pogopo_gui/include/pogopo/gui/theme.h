#pragma once

#include "pogopo/gfx/gfx.h"

namespace pogopo::gui {

struct Theme {
    gfx::Color foreground = gfx::BLACK;
    gfx::Color background = gfx::WHITE;
    gfx::Color border = gfx::BLACK;
    gfx::Color focus_foreground = gfx::WHITE;
    gfx::Color focus_background = gfx::BLACK;
    int padding = 4;
    int row_height = 22;
};

inline constexpr Theme kDefaultTheme{};

} // namespace pogopo::gui

