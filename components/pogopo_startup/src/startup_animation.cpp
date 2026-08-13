#include "pogopo/startup/startup_animation.h"

#include <cstring>

#include "pogopo/gfx/graphics.h"

extern const uint8_t startup_frames_start[]
    asm("_binary_startup_frames_bin_start");
extern const uint8_t startup_frames_end[]
    asm("_binary_startup_frames_bin_end");
extern const uint8_t outro_frames_start[]
    asm("_binary_outro_frames_bin_start");
extern const uint8_t outro_frames_end[]
    asm("_binary_outro_frames_bin_end");

namespace pogopo::startup {

size_t StartupAnimation::embeddedSize() const {
    return reinterpret_cast<uintptr_t>(startup_frames_end) -
           reinterpret_cast<uintptr_t>(startup_frames_start);
}

bool StartupAnimation::valid() const {
    return embeddedSize() == FRAME_COUNT * FRAME_SIZE;
}

esp_err_t StartupAnimation::show(gfx::Graphics& graphics, size_t frame_index) const {
    if (!valid() || frame_index >= FRAME_COUNT) return ESP_ERR_INVALID_SIZE;
    if (!graphics.ok() || graphics.width() != WIDTH || graphics.height() != HEIGHT) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t* frame = startup_frames_start + frame_index * FRAME_SIZE;
    const esp_err_t load_error = graphics.display().load_framebuffer(frame, FRAME_SIZE);
    if (load_error != ESP_OK) return load_error;
    return graphics.presentFull();
}

size_t OutroAnimation::embeddedSize() const {
    return reinterpret_cast<uintptr_t>(outro_frames_end) -
           reinterpret_cast<uintptr_t>(outro_frames_start);
}

bool OutroAnimation::valid() const {
    return embeddedSize() == FRAME_COUNT * FRAME_SIZE;
}

esp_err_t OutroAnimation::show(gfx::Graphics& graphics, size_t frame_index) const {
    if (!valid() || frame_index >= FRAME_COUNT) return ESP_ERR_INVALID_SIZE;
    if (!graphics.ok() || graphics.width() != WIDTH || graphics.height() != HEIGHT) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t* frame = outro_frames_start + frame_index * FRAME_SIZE;
    const esp_err_t load_error = graphics.display().load_framebuffer(frame, FRAME_SIZE);
    if (load_error != ESP_OK) return load_error;
    return graphics.presentFull();
}

} // namespace pogopo::startup
