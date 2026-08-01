#pragma once

#include "pogopo/gfx/gfx.h"
#include "pogopo/gui/theme.h"

namespace pogopo::gui {

class Widget {
public:
    explicit Widget(gfx::Rect bounds = {}) : bounds_(bounds) {}
    virtual ~Widget() = default;

    virtual void draw(gfx::Canvas& canvas, const Theme& theme) = 0;

    void setBounds(const gfx::Rect& bounds) { bounds_ = bounds; }
    gfx::Rect bounds() const { return bounds_; }

    void setVisible(bool visible) { visible_ = visible; }
    bool visible() const { return visible_; }

    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

protected:
    gfx::Rect bounds_{};
    bool visible_ = true;
    bool enabled_ = true;
};

} // namespace pogopo::gui

