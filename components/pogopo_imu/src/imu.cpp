#include "pogopo/imu/imu.h"

#include <cmath>

#include "driver/gpio.h"
#include "esp_log.h"

namespace pogopo::imu {
namespace { constexpr char TAG[] = "pogopo_imu"; constexpr float RAD_TO_DEG = 57.2957795131f; }

Imu::~Imu() { end(); }

esp_err_t Imu::begin(const Config& config) {
    if (task_ || handle_) return ESP_ERR_INVALID_STATE;
    if (!config.bus || config.sample_period_ms < 5) return ESP_ERR_INVALID_ARG;
    config_ = config;
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return ESP_ERR_NO_MEM;

    if (config.interrupt_io >= 0) {
        gpio_config_t io{};
        io.pin_bit_mask = 1ULL << config.interrupt_io;
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io); // reserved for a future data-ready interrupt mode
    }

    bmi270_driver_config_t driver{};
    driver.addr = config.address;
    driver.interface = BMI270_USE_I2C;
    driver.i2c_bus = config.bus;
    esp_err_t err = bmi270_create(&driver, &handle_);
    if (err != ESP_OK) { vSemaphoreDelete(mutex_); mutex_ = nullptr; return err; }
    err = bmi270_get_chip_id(handle_, &chip_id_);
    if (err != ESP_OK || chip_id_ != BMI270_CHIP_ID) {
        bmi270_delete(handle_); handle_ = nullptr;
        vSemaphoreDelete(mutex_); mutex_ = nullptr;
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }

    bmi270_config_t measurement{};
    measurement.acce_odr = BMI270_ACC_ODR_100_HZ;
    measurement.acce_range = BMI270_ACC_RANGE_2_G;
    measurement.gyro_odr = BMI270_GYR_ODR_100_HZ;
    measurement.gyro_range = BMI270_GYR_RANGE_2000_DPS;
    err = bmi270_start(handle_, &measurement);
    if (err != ESP_OK) {
        bmi270_delete(handle_); handle_ = nullptr;
        vSemaphoreDelete(mutex_); mutex_ = nullptr;
        return err;
    }

    if (xTaskCreatePinnedToCore(taskEntry, "pogopo_imu", config.task_stack, this,
                                config.task_priority, &task_, config.task_core) != pdPASS) {
        bmi270_stop(handle_); bmi270_delete(handle_); handle_ = nullptr;
        vSemaphoreDelete(mutex_); mutex_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ok_.store(true);
    ESP_LOGI(TAG, "BMI270 ready: ID=0x%02X, accel 2g, gyro 2000dps, 100Hz", chip_id_);
    return ESP_OK;
}

void Imu::end() {
    ok_.store(false);
    if (task_) { vTaskDelete(task_); task_ = nullptr; }
    if (handle_) { bmi270_stop(handle_); bmi270_delete(handle_); handle_ = nullptr; }
    if (mutex_) { vSemaphoreDelete(mutex_); mutex_ = nullptr; }
}

Sample Imu::sample() const {
    Sample copy{};
    if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(5)) == pdTRUE) {
        copy = sample_;
        xSemaphoreGive(mutex_);
    }
    return copy;
}

void Imu::taskEntry(void* arg) { static_cast<Imu*>(arg)->taskLoop(); }

void Imu::taskLoop() {
    TickType_t wake = xTaskGetTickCount();
    uint32_t errors = 0, sequence = 0;
    while (true) {
        Sample next{};
        const esp_err_t a = bmi270_get_acce_data(handle_, &next.ax, &next.ay, &next.az);
        const esp_err_t g = bmi270_get_gyro_data(handle_, &next.gx, &next.gy, &next.gz);
        next.valid = a == ESP_OK && g == ESP_OK;
        if (!next.valid) ++errors;
        else {
            next.roll = std::atan2(next.ay, next.az) * RAD_TO_DEG;
            next.pitch = std::atan2(-next.ax, std::sqrt(next.ay * next.ay + next.az * next.az)) * RAD_TO_DEG;
        }
        next.read_errors = errors;
        next.sequence = ++sequence;
        if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(5)) == pdTRUE) {
            sample_ = next;
            xSemaphoreGive(mutex_);
        }
        ok_.store(next.valid || errors < 5);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(config_.sample_period_ms));
    }
}

} // namespace pogopo::imu
