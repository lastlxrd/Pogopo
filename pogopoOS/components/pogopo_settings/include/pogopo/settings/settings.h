#pragma once

#include <atomic>
#include <cstdint>

#include "esp_err.h"
#include "nvs.h"

namespace pogopo::settings {

class Settings {
public:
    struct Defaults {
        uint8_t volume = 68;
        bool audio_enabled = true;
        bool ui_sounds_enabled = true;
        bool haptics_enabled = true;
        uint8_t motion_sensitivity = 1; // 0 low, 1 normal, 2 high
    };

    Settings() = default;
    ~Settings();
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    esp_err_t begin();
    esp_err_t begin(const char* name_space);
    esp_err_t begin(const char* name_space, const Defaults& defaults);
    void end();
    esp_err_t save();
    esp_err_t reload();
    esp_err_t resetDefaults(bool persist = true);

    bool ok() const { return ok_.load(); }
    bool dirty() const { return dirty_.load(); }

    uint8_t volume() const { return volume_.load(); }
    bool audioEnabled() const { return audio_enabled_.load(); }
    bool uiSoundsEnabled() const { return ui_sounds_enabled_.load(); }
    bool hapticsEnabled() const { return haptics_enabled_.load(); }
    uint8_t motionSensitivity() const { return motion_sensitivity_.load(); }

    void setVolume(uint8_t value);
    void setAudioEnabled(bool value);
    void setUiSoundsEnabled(bool value);
    void setHapticsEnabled(bool value);
    void setMotionSensitivity(uint8_t value);

private:
    void applyDefaults();
    esp_err_t loadValues();

    Defaults defaults_{};
    nvs_handle_t handle_ = 0;
    std::atomic<bool> ok_{false};
    std::atomic<bool> dirty_{false};
    std::atomic<uint8_t> volume_{68};
    std::atomic<bool> audio_enabled_{true};
    std::atomic<bool> ui_sounds_enabled_{true};
    std::atomic<bool> haptics_enabled_{true};
    std::atomic<uint8_t> motion_sensitivity_{1};
};

const char* motion_sensitivity_name(uint8_t value);

} // namespace pogopo::settings

