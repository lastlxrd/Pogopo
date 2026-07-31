#pragma once

#include <atomic>
#include <cstdint>

#include "bmi270.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace pogopo::imu {

struct Sample {
    float ax = 0, ay = 0, az = 0;
    float gx = 0, gy = 0, gz = 0;
    float roll = 0, pitch = 0;
    uint32_t sequence = 0;
    uint32_t read_errors = 0;
    bool valid = false;
};

class Imu {
public:
    struct Config {
        i2c_master_bus_handle_t bus = nullptr;
        uint8_t address = 0x68;
        int interrupt_io = 18;
        uint16_t sample_period_ms = 20;
        uint32_t task_stack = 6144;
        UBaseType_t task_priority = 4;
        BaseType_t task_core = 1;
    };

    Imu() = default;
    ~Imu();
    Imu(const Imu&) = delete;
    Imu& operator=(const Imu&) = delete;

    esp_err_t begin(const Config& config);
    void end();
    Sample sample() const;
    bool ok() const { return ok_.load(); }
    uint8_t chipId() const { return chip_id_; }

private:
    static void taskEntry(void* arg);
    void taskLoop();

    Config config_{};
    bmi270_handle_t* handle_ = nullptr;
    TaskHandle_t task_ = nullptr;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    Sample sample_{};
    std::atomic<bool> ok_{false};
    uint8_t chip_id_ = 0;
};

} // namespace pogopo::imu
