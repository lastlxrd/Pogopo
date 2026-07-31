#pragma once

#include "pogopo/gui/widget.h"

namespace pogopo::gui {

class Dialog final : public Widget {
public:
    explicit Dialog(gfx::Rect bounds = {}) : Widget(bounds) {}

    void setTitle(const char* title) { title_ = title ? title : ""; }
    void setMessage(const char* message) { message_ = message ? message : ""; }
    void setFooter(const char* footer) { footer_ = footer ? footer : ""; }
    void draw(gfx::Canvas& canvas, const Theme& theme) override;

private:
    const char* title_ = "";
    const char* message_ = "";
    const char* footer_ = "";
};

} // namespace pogopo::gui
