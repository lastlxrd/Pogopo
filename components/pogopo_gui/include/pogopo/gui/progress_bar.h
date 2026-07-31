#pragma once

#include "pogopo/gui/widget.h"

namespace pogopo::gui {

class ProgressBar final : public Widget {
public:
    explicit ProgressBar(gfx::Rect bounds = {}) : Widget(bounds) {}

    void setRange(int minimum, int maximum);
    void setValue(int value);
    int value() const { return value_; }
    void setShowValue(bool show) { show_value_ = show; }
    void draw(gfx::Canvas& canvas, const Theme& theme) override;

private:
    int minimum_ = 0;
    int maximum_ = 100;
    int value_ = 0;
    bool show_value_ = false;
};

} // namespace pogopo::gui
