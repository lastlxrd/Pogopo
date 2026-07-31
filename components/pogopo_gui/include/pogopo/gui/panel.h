#pragma once

#include "pogopo/gui/widget.h"

namespace pogopo::gui {

class Panel final : public Widget {
public:
    explicit Panel(gfx::Rect bounds = {}) : Widget(bounds) {}

    void setFilled(bool filled) { filled_ = filled; }
    void setInverted(bool inverted) { inverted_ = inverted; }
    void draw(gfx::Canvas& canvas, const Theme& theme) override;

private:
    bool filled_ = true;
    bool inverted_ = false;
};

} // namespace pogopo::gui
