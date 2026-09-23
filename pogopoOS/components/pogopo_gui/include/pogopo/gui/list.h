#pragma once

#include <cstddef>
#include "pogopo/gui/widget.h"

namespace pogopo::gui {

struct ListItem {
    const char* label = "";
    const char* subtitle = nullptr;
    const char* id = nullptr;
    bool enabled = true;
};

class List final : public Widget {
public:
    explicit List(gfx::Rect bounds = {}) : Widget(bounds) {}

    void setItems(const ListItem* items, size_t count);
    void setSelected(size_t index);
    size_t selected() const { return selected_; }
    size_t count() const { return count_; }
    bool move(int delta);
    const ListItem* selectedItem() const;
    void setRowHeight(int row_height) { row_height_ = row_height < 12 ? 12 : row_height; }
    int rowHeight() const { return row_height_; }
    gfx::Rect rowsBounds() const { return bounds_; }

    void draw(gfx::Canvas& canvas, const Theme& theme) override;

private:
    void ensureVisible();

    const ListItem* items_ = nullptr;
    size_t count_ = 0;
    size_t selected_ = 0;
    size_t first_visible_ = 0;
    int row_height_ = 22;
};

} // namespace pogopo::gui

