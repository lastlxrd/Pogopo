#pragma once

#include "pogopo/gui/theme.h"
#include "pogopo/gui/widget.h"
#include "pogopo/gui/label.h"
#include "pogopo/gui/panel.h"
#include "pogopo/gui/progress_bar.h"
#include "pogopo/gui/list.h"
#include "pogopo/gui/dialog.h"

namespace pogopo::gui {

int text_width(const char* text, int scale = 1,
               const gfx::Font& font = gfx::font5x7());
int centered_text_x(const gfx::Rect& bounds, const char* text, int scale = 1,
                    const gfx::Font& font = gfx::font5x7());
void draw_header(gfx::Canvas& canvas, const Theme& theme,
                 const char* title, const char* right_text = nullptr);
void draw_footer(gfx::Canvas& canvas, const Theme& theme,
                 const char* left_text, const char* right_text = nullptr);
void draw_wrapped_text(gfx::Canvas& canvas, const gfx::Rect& bounds,
                       const char* text, const Theme& theme,
                       int scale = 1, int line_spacing = 2);

} // namespace pogopo::gui

namespace pogopo {
using GuiTheme = gui::Theme;
using GuiWidget = gui::Widget;
using GuiLabel = gui::Label;
using GuiPanel = gui::Panel;
using GuiProgressBar = gui::ProgressBar;
using GuiList = gui::List;
using GuiListItem = gui::ListItem;
using GuiDialog = gui::Dialog;
} // namespace pogopo

