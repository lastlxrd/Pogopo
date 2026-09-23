#pragma once

#include "pogopo/gui/widget.h"

namespace pogopo::gui {

class Label final : public Widget {
public:
    Label(gfx::Rect bounds = {}, const char* text = "") : Widget(bounds), text_(text) {}

    void setText(const char* text) { text_ = text ? text : ""; }
    const char* text() const { return text_; }

    void setScale(int scale) { scale_ = scale < 1 ? 1 : scale; }
    void setInverted(bool inverted) { inverted_ = inverted; }

    void draw(gfx::Canvas& canvas, const Theme& theme) override;

private:
    const char* text_ = "";
    int scale_ = 1;
    bool inverted_ = false;
};

} // namespace pogopo::gui

