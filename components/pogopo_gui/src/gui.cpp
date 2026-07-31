#include "pogopo/gui/gui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pogopo::gui {

namespace {
constexpr int kHeaderHeight = 26;
constexpr int kFooterHeight = 17;

size_t next_enabled(const ListItem* items, size_t count, size_t from, int direction) {
    if (!items || count == 0 || direction == 0) return from;
    size_t index = from;
    for (size_t attempt = 0; attempt < count; ++attempt) {
        if (direction > 0) index = (index + 1) % count;
        else index = index == 0 ? count - 1 : index - 1;
        if (items[index].enabled) return index;
    }
    return from;
}
} // namespace

int text_width(const char* text, int scale, const gfx::Font& font) {
    if (!text || !*text) return 0;
    scale = std::max(1, scale);
    int current = 0;
    int widest = 0;
    for (const char* p = text; *p; ++p) {
        if (*p == '\n') {
            widest = std::max(widest, current);
            current = 0;
        } else {
            current += (font.glyph_width + font.spacing) * scale;
        }
    }
    widest = std::max(widest, current);
    return std::max(0, widest - font.spacing * scale);
}

int centered_text_x(const gfx::Rect& bounds, const char* text, int scale,
                    const gfx::Font& font) {
    return bounds.x + std::max(0, (bounds.w - text_width(text, scale, font)) / 2);
}

void Label::draw(gfx::Canvas& canvas, const Theme& theme) {
    if (!visible_ || bounds_.empty()) return;
    const gfx::Color fg = inverted_ ? theme.focus_foreground : theme.foreground;
    const gfx::Color bg = inverted_ ? theme.focus_background : theme.background;
    if (inverted_) canvas.fill_rect(bounds_.x, bounds_.y, bounds_.w, bounds_.h, bg);
    canvas.draw_text(bounds_.x, bounds_.y, text_, gfx::font5x7(), fg, scale_, true, bg);
}

void Panel::draw(gfx::Canvas& canvas, const Theme& theme) {
    if (!visible_ || bounds_.empty()) return;
    const gfx::Color fg = inverted_ ? theme.focus_foreground : theme.foreground;
    const gfx::Color bg = inverted_ ? theme.focus_background : theme.background;
    if (filled_) canvas.fill_rect(bounds_.x, bounds_.y, bounds_.w, bounds_.h, bg);
    canvas.draw_rect(bounds_.x, bounds_.y, bounds_.w, bounds_.h, fg);
}

void ProgressBar::setRange(int minimum, int maximum) {
    minimum_ = minimum;
    maximum_ = std::max(minimum + 1, maximum);
    setValue(value_);
}

void ProgressBar::setValue(int value) {
    value_ = std::clamp(value, minimum_, maximum_);
}

void ProgressBar::draw(gfx::Canvas& canvas, const Theme& theme) {
    if (!visible_ || bounds_.empty()) return;
    canvas.fill_rect(bounds_.x, bounds_.y, bounds_.w, bounds_.h, theme.background);
    canvas.draw_rect(bounds_.x, bounds_.y, bounds_.w, bounds_.h, theme.border);
    const int inner_w = std::max(0, bounds_.w - 4);
    const int inner_h = std::max(0, bounds_.h - 4);
    const int range = maximum_ - minimum_;
    const int filled = range > 0 ? (inner_w * (value_ - minimum_)) / range : 0;
    if (filled > 0 && inner_h > 0) {
        canvas.fill_rect(bounds_.x + 2, bounds_.y + 2, filled, inner_h, theme.foreground);
    }
    if (show_value_) {
        char value[16];
        std::snprintf(value, sizeof(value), "%d%%", ((value_ - minimum_) * 100) / range);
        const int tx = centered_text_x(bounds_, value, 1);
        const int ty = bounds_.y + std::max(1, (bounds_.h - 7) / 2);
        canvas.draw_text(tx, ty, value, gfx::font5x7(), theme.foreground, 1, true, theme.background);
    }
}

void List::setItems(const ListItem* items, size_t count) {
    items_ = items;
    count_ = count;
    selected_ = 0;
    first_visible_ = 0;
    if (items_ && count_ > 0 && !items_[selected_].enabled) {
        selected_ = next_enabled(items_, count_, selected_, 1);
    }
}

void List::setSelected(size_t index) {
    if (!items_ || count_ == 0) {
        selected_ = 0;
        first_visible_ = 0;
        return;
    }
    selected_ = std::min(index, count_ - 1);
    if (!items_[selected_].enabled) selected_ = next_enabled(items_, count_, selected_, 1);
    ensureVisible();
}

bool List::move(int delta) {
    if (!items_ || count_ == 0 || delta == 0) return false;
    const size_t old = selected_;
    const int direction = delta > 0 ? 1 : -1;
    for (int step = 0; step < std::abs(delta); ++step) {
        selected_ = next_enabled(items_, count_, selected_, direction);
    }
    ensureVisible();
    return old != selected_;
}

const ListItem* List::selectedItem() const {
    if (!items_ || count_ == 0 || selected_ >= count_) return nullptr;
    return &items_[selected_];
}

void List::ensureVisible() {
    if (row_height_ <= 0 || bounds_.h <= 0) return;
    const size_t visible_rows = std::max<size_t>(1, static_cast<size_t>(bounds_.h / row_height_));
    if (selected_ < first_visible_) first_visible_ = selected_;
    if (selected_ >= first_visible_ + visible_rows) first_visible_ = selected_ - visible_rows + 1;
}

void List::draw(gfx::Canvas& canvas, const Theme& theme) {
    if (!visible_ || bounds_.empty()) return;
    canvas.fill_rect(bounds_.x, bounds_.y, bounds_.w, bounds_.h, theme.background);
    canvas.draw_rect(bounds_.x, bounds_.y, bounds_.w, bounds_.h, theme.border);
    if (!items_ || count_ == 0) return;

    const gfx::Rect previous_clip = canvas.clip();
    const gfx::Rect inner = gfx::Rect::intersect({bounds_.x + 1, bounds_.y + 1,
                                                  bounds_.w - 2, bounds_.h - 2},
                                                 previous_clip);
    canvas.set_clip(inner);
    const size_t visible_rows = std::max<size_t>(1, static_cast<size_t>((bounds_.h - 2) / row_height_));
    for (size_t row = 0; row < visible_rows; ++row) {
        const size_t index = first_visible_ + row;
        if (index >= count_) break;
        const int y = bounds_.y + 1 + static_cast<int>(row) * row_height_;
        const bool selected = index == selected_;
        const bool enabled = items_[index].enabled;
        const gfx::Color bg = selected ? theme.focus_background : theme.background;
        const gfx::Color fg = selected ? theme.focus_foreground : theme.foreground;
        canvas.fill_rect(bounds_.x + 1, y, bounds_.w - 2, row_height_, bg);
        if (selected) canvas.draw_rect(bounds_.x + 3, y + 2, bounds_.w - 6, row_height_ - 4, fg);
        canvas.draw_text(bounds_.x + 10, y + 5, items_[index].label ? items_[index].label : "",
                         gfx::font5x7(), enabled ? fg : theme.border, 1, true, bg);
        if (items_[index].subtitle && row_height_ >= 25) {
            canvas.draw_text(bounds_.x + 10, y + 15, items_[index].subtitle,
                             gfx::font5x7(), enabled ? fg : theme.border, 1, true, bg);
        }
        if (selected) canvas.draw_text(bounds_.right() - 15, y + 5, ">", gfx::font5x7(), fg, 1, true, bg);
    }
    canvas.set_clip(previous_clip);
}

void Dialog::draw(gfx::Canvas& canvas, const Theme& theme) {
    if (!visible_ || bounds_.empty()) return;
    canvas.fill_rect(bounds_.x, bounds_.y, bounds_.w, bounds_.h, theme.background);
    canvas.draw_rect(bounds_.x, bounds_.y, bounds_.w, bounds_.h, theme.border);
    canvas.draw_rect(bounds_.x + 2, bounds_.y + 2, bounds_.w - 4, bounds_.h - 4, theme.border);
    canvas.fill_rect(bounds_.x + 4, bounds_.y + 4, bounds_.w - 8, 20, theme.focus_background);
    canvas.draw_text(bounds_.x + 10, bounds_.y + 10, title_, gfx::font5x7(),
                     theme.focus_foreground, 1, true, theme.focus_background);
    draw_wrapped_text(canvas, {bounds_.x + 10, bounds_.y + 31, bounds_.w - 20, bounds_.h - 52},
                      message_, theme, 1, 2);
    if (footer_ && *footer_) {
        const int x = centered_text_x({bounds_.x + 4, bounds_.bottom() - 17, bounds_.w - 8, 12}, footer_, 1);
        canvas.draw_text(x, bounds_.bottom() - 14, footer_, gfx::font5x7(), theme.foreground, 1, true, theme.background);
    }
}

void draw_header(gfx::Canvas& canvas, const Theme& theme,
                 const char* title, const char* right_text) {
    canvas.fill_rect(0, 0, canvas.width(), kHeaderHeight, theme.focus_background);
    canvas.draw_text(9, 9, title ? title : "", gfx::font5x7(), theme.focus_foreground, 1,
                     true, theme.focus_background);
    if (right_text && *right_text) {
        const int x = canvas.width() - text_width(right_text, 1) - 9;
        canvas.draw_text(x, 9, right_text, gfx::font5x7(), theme.focus_foreground, 1,
                         true, theme.focus_background);
    }
}

void draw_footer(gfx::Canvas& canvas, const Theme& theme,
                 const char* left_text, const char* right_text) {
    const int y = canvas.height() - kFooterHeight;
    canvas.fill_rect(0, y, canvas.width(), kFooterHeight, theme.focus_background);
    canvas.draw_text(8, y + 5, left_text ? left_text : "", gfx::font5x7(),
                     theme.focus_foreground, 1, true, theme.focus_background);
    if (right_text && *right_text) {
        const int x = canvas.width() - text_width(right_text, 1) - 8;
        canvas.draw_text(x, y + 5, right_text, gfx::font5x7(),
                         theme.focus_foreground, 1, true, theme.focus_background);
    }
}

void draw_wrapped_text(gfx::Canvas& canvas, const gfx::Rect& bounds,
                       const char* text, const Theme& theme,
                       int scale, int line_spacing) {
    if (!text || bounds.empty()) return;
    scale = std::max(1, scale);
    const int char_width = (gfx::font5x7().glyph_width + gfx::font5x7().spacing) * scale;
    const int line_height = gfx::font5x7().glyph_height * scale + line_spacing;
    const int max_chars = std::max(1, bounds.w / char_width);
    char line[96] = {};
    int line_length = 0;
    int y = bounds.y;
    const char* p = text;

    while (*p && y + gfx::font5x7().glyph_height * scale <= bounds.bottom()) {
        if (*p == '\n') {
            line[line_length] = '\0';
            canvas.draw_text(bounds.x, y, line, gfx::font5x7(), theme.foreground, scale, true, theme.background);
            line_length = 0;
            ++p;
            y += line_height;
            continue;
        }

        const char* word_start = p;
        int word_length = 0;
        while (p[word_length] && p[word_length] != ' ' && p[word_length] != '\n') ++word_length;
        const int needed = word_length + (line_length > 0 ? 1 : 0);
        if (line_length > 0 && line_length + needed > max_chars) {
            line[line_length] = '\0';
            canvas.draw_text(bounds.x, y, line, gfx::font5x7(), theme.foreground, scale, true, theme.background);
            line_length = 0;
            y += line_height;
            if (y + gfx::font5x7().glyph_height * scale > bounds.bottom()) break;
        }
        if (line_length > 0 && line_length < static_cast<int>(sizeof(line)) - 1) line[line_length++] = ' ';
        for (int i = 0; i < word_length && line_length < static_cast<int>(sizeof(line)) - 1; ++i) {
            line[line_length++] = word_start[i];
        }
        p += word_length;
        while (*p == ' ') ++p;
    }

    if (line_length > 0 && y + gfx::font5x7().glyph_height * scale <= bounds.bottom()) {
        line[line_length] = '\0';
        canvas.draw_text(bounds.x, y, line, gfx::font5x7(), theme.foreground, scale, true, theme.background);
    }
}

} // namespace pogopo::gui
