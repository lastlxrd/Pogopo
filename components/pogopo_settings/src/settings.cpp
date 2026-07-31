#include "pogopo/settings/settings.h"

#include <algorithm>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"

namespace pogopo::settings {
namespace {
constexpr char TAG[] = "pogopo_settings";
constexpr char KEY_VOLUME[] = "volume";
constexpr char KEY_AUDIO[] = "audio";
constexpr char KEY_UI_SOUNDS[] = "ui_sounds";
constexpr char KEY_HAPTICS[] = "haptics";
constexpr char KEY_MOTION[] = "motion";
}

Settings::~Settings() {
    end();
}

esp_err_t Settings::begin() {
    return begin("pogopo", Defaults{});
}

esp_err_t Settings::begin(const char* name_space) {
    return begin(name_space, Defaults{});
}

esp_err_t Settings::begin(const char* name_space, const Defaults& defaults) {
    if (ok_.load() || handle_ != 0 || !name_space) {
        return ESP_ERR_INVALID_STATE;
    }

    defaults_ = defaults;
    defaults_.volume = std::min<uint8_t>(defaults_.volume, 100);
    defaults_.motion_sensitivity = std::min<uint8_t>(defaults_.motion_sensitivity, 2);
    applyDefaults();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Could not erase invalid NVS");
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_open(name_space, NVS_READWRITE, &handle_);
    if (err != ESP_OK) {
        handle_ = 0;
        return err;
    }

    ok_.store(true);
    err = loadValues();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Settings load returned %s; defaults remain active", esp_err_to_name(err));
    }
    dirty_.store(false);

    ESP_LOGI(TAG, "Settings: volume=%u audio=%u ui=%u haptics=%u motion=%u",
             static_cast<unsigned>(volume()),
             audioEnabled() ? 1U : 0U,
             uiSoundsEnabled() ? 1U : 0U,
             hapticsEnabled() ? 1U : 0U,
             static_cast<unsigned>(motionSensitivity()));
    return ESP_OK;
}

void Settings::end() {
    if (handle_ != 0) {
        if (dirty_.load()) {
            save();
        }
        nvs_close(handle_);
        handle_ = 0;
    }
    ok_.store(false);
}

esp_err_t Settings::save() {
    if (!ok_.load() || handle_ == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(nvs_set_u8(handle_, KEY_VOLUME, volume()), TAG, "Save volume failed");
    ESP_RETURN_ON_ERROR(nvs_set_u8(handle_, KEY_AUDIO, audioEnabled() ? 1U : 0U), TAG, "Save audio failed");
    ESP_RETURN_ON_ERROR(nvs_set_u8(handle_, KEY_UI_SOUNDS, uiSoundsEnabled() ? 1U : 0U), TAG, "Save UI sounds failed");
    ESP_RETURN_ON_ERROR(nvs_set_u8(handle_, KEY_HAPTICS, hapticsEnabled() ? 1U : 0U), TAG, "Save haptics failed");
    ESP_RETURN_ON_ERROR(nvs_set_u8(handle_, KEY_MOTION, motionSensitivity()), TAG, "Save motion failed");
    ESP_RETURN_ON_ERROR(nvs_commit(handle_), TAG, "NVS commit failed");
    dirty_.store(false);
    return ESP_OK;
}

esp_err_t Settings::reload() {
    if (!ok_.load() || handle_ == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    applyDefaults();
    const esp_err_t err = loadValues();
    dirty_.store(false);
    return err;
}

esp_err_t Settings::resetDefaults(bool persist) {
    applyDefaults();
    dirty_.store(true);
    return persist ? save() : ESP_OK;
}

void Settings::setVolume(uint8_t value) {
    value = std::min<uint8_t>(value, 100);
    if (volume_.exchange(value) != value) {
        dirty_.store(true);
    }
}

void Settings::setAudioEnabled(bool value) {
    if (audio_enabled_.exchange(value) != value) {
        dirty_.store(true);
    }
}

void Settings::setUiSoundsEnabled(bool value) {
    if (ui_sounds_enabled_.exchange(value) != value) {
        dirty_.store(true);
    }
}

void Settings::setHapticsEnabled(bool value) {
    if (haptics_enabled_.exchange(value) != value) {
        dirty_.store(true);
    }
}

void Settings::setMotionSensitivity(uint8_t value) {
    value = std::min<uint8_t>(value, 2);
    if (motion_sensitivity_.exchange(value) != value) {
        dirty_.store(true);
    }
}

void Settings::applyDefaults() {
    volume_.store(defaults_.volume);
    audio_enabled_.store(defaults_.audio_enabled);
    ui_sounds_enabled_.store(defaults_.ui_sounds_enabled);
    haptics_enabled_.store(defaults_.haptics_enabled);
    motion_sensitivity_.store(defaults_.motion_sensitivity);
}

esp_err_t Settings::loadValues() {
    uint8_t value = 0;
    esp_err_t first_error = ESP_OK;

    esp_err_t err = nvs_get_u8(handle_, KEY_VOLUME, &value);
    if (err == ESP_OK) {
        volume_.store(std::min<uint8_t>(value, 100));
    } else if (err != ESP_ERR_NVS_NOT_FOUND && first_error == ESP_OK) {
        first_error = err;
    }

    err = nvs_get_u8(handle_, KEY_AUDIO, &value);
    if (err == ESP_OK) {
        audio_enabled_.store(value != 0);
    } else if (err != ESP_ERR_NVS_NOT_FOUND && first_error == ESP_OK) {
        first_error = err;
    }

    err = nvs_get_u8(handle_, KEY_UI_SOUNDS, &value);
    if (err == ESP_OK) {
        ui_sounds_enabled_.store(value != 0);
    } else if (err != ESP_ERR_NVS_NOT_FOUND && first_error == ESP_OK) {
        first_error = err;
    }

    err = nvs_get_u8(handle_, KEY_HAPTICS, &value);
    if (err == ESP_OK) {
        haptics_enabled_.store(value != 0);
    } else if (err != ESP_ERR_NVS_NOT_FOUND && first_error == ESP_OK) {
        first_error = err;
    }

    err = nvs_get_u8(handle_, KEY_MOTION, &value);
    if (err == ESP_OK) {
        motion_sensitivity_.store(std::min<uint8_t>(value, 2));
    } else if (err != ESP_ERR_NVS_NOT_FOUND && first_error == ESP_OK) {
        first_error = err;
    }

    return first_error;
}

const char* motion_sensitivity_name(uint8_t value) {
    switch (value) {
        case 0:
            return "LOW";
        case 1:
            return "NORMAL";
        case 2:
            return "HIGH";
        default:
            return "?";
    }
}

} // namespace pogopo::settings
