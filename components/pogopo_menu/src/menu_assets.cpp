#include "pogopo/menu/menu_assets.h"

#include <algorithm>

#include "menu_data_generated.h"

extern const uint8_t menu_assets_start[] asm("_binary_menu_assets_bin_start");
extern const uint8_t menu_assets_end[] asm("_binary_menu_assets_bin_end");

namespace pogopo::menu {

namespace {

const generated::AnimationMeta* metadata(Art art) {
    const size_t index = static_cast<size_t>(art);
    if (index >= static_cast<size_t>(Art::Count)) return nullptr;
    return &generated::kAnimations[index];
}

} // namespace

size_t Assets::embeddedSize() {
    return reinterpret_cast<uintptr_t>(menu_assets_end) -
           reinterpret_cast<uintptr_t>(menu_assets_start);
}

bool Assets::valid() {
    return embeddedSize() == generated::kBlobSize;
}

const uint8_t* Assets::base() {
    return valid() ? menu_assets_start : nullptr;
}

AnimationInfo Assets::info(Art art) {
    const auto* meta = metadata(art);
    if (!meta) return {};
    return {meta->width, meta->height, meta->frame_count,
            meta->source_x, meta->source_y};
}

gfx::Bitmap Assets::frame(Art art, size_t frame_index) {
    const auto* meta = metadata(art);
    const uint8_t* data = base();
    if (!meta || !data || frame_index >= meta->frame_count) return {};
    const size_t frame_size = static_cast<size_t>(meta->stride) * meta->height;
    return gfx::make_bitmap_1bpp(
        meta->width, meta->height,
        data + meta->offset + frame_index * frame_size,
        gfx::BitOrder::MSB_FIRST, meta->stride);
}

uint16_t Assets::frameDuration(Art art, size_t frame_index) {
    const auto* meta = metadata(art);
    if (!meta || frame_index >= meta->frame_count) return 100;
    return meta->durations_ms[frame_index];
}

size_t Assets::frameAtTime(Art art, uint32_t elapsed_ms) {
    const auto* meta = metadata(art);
    if (!meta || meta->frame_count == 0) return 0;
    uint32_t total = 0;
    for (size_t i = 0; i < meta->frame_count; ++i) {
        total += std::max<uint16_t>(meta->durations_ms[i], 1);
    }
    if (total == 0) return 0;
    uint32_t position = elapsed_ms % total;
    for (size_t i = 0; i < meta->frame_count; ++i) {
        const uint16_t duration = std::max<uint16_t>(meta->durations_ms[i], 1);
        if (position < duration) return i;
        position -= duration;
    }
    return meta->frame_count - 1;
}

} // namespace pogopo::menu
