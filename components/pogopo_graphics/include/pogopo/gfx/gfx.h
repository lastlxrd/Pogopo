#pragma once

#include "pogopo/gfx/types.h"
#include "pogopo/gfx/bitmap.h"
#include "pogopo/gfx/sprite.h"
#include "pogopo/gfx/font.h"
#include "pogopo/gfx/sharp_display.h"
#include "pogopo/gfx/canvas.h"
#include "pogopo/gfx/graphics.h"

// Friendly top-level aliases for application code:
// pogopo::Graphics, pogopo::Canvas, pogopo::Bitmap, ...
namespace pogopo {
using Graphics = gfx::Graphics;
using Canvas = gfx::Canvas;
using SharpDisplay = gfx::SharpDisplay;
using Bitmap = gfx::Bitmap;
using Sprite = gfx::Sprite;
using Font = gfx::Font;
using Rect = gfx::Rect;
using Point = gfx::Point;
using Color = gfx::Color;
inline constexpr Color BLACK = gfx::BLACK;
inline constexpr Color WHITE = gfx::WHITE;
} // namespace pogopo

