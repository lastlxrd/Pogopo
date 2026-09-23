#include "pogopo/gfx/graphics.h"

namespace pogopo::gfx {

Graphics::Graphics() : canvas_(display_) {}

esp_err_t Graphics::begin() {
    Config config;
    return begin(config);
}

esp_err_t Graphics::begin(const Config& config) {
    const esp_err_t err = display_.init(config);
    if (err == ESP_OK) {
        canvas_.reset_clip();
    }
    return err;
}

void Graphics::end() {
    display_.deinit();
}

} // namespace pogopo::gfx

