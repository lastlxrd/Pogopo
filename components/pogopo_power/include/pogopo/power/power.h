#pragma once

#include <atomic>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace pogopo::power {

enum class EventType : uint8_t { UsbBlocked, ShutdownRequested };
struct Event { EventType type = EventType::UsbBlocked; };

struct State {
    bool ok = false;
    bool usb_present = false;
    bool charging = false;
    bool power_button_down = false;
    bool battery_valid = false;
    uint16_t battery_mv = 0;
    uint8_t battery_percent = 0;
    uint8_t hold_percent = 0;
    uint8_t reg08 = 0;
    uint8_t reg09 = 0;
    uint32_t i2c_errors = 0;
    uint32_t sequence = 0;
};

class Power {
public:
    struct Config {
        i2c_master_bus_handle_t bus = nullptr;
        uint8_t charger_address = 0x6B;
        int power_button_io = 17;
        int charger_int_io = 41;
        int battery_measure_io = 1;
        int battery_gate_io = 2;
        adc_unit_t adc_unit = ADC_UNIT_1;
        adc_channel_t adc_channel = ADC_CHANNEL_0;
        uint16_t shutdown_hold_ms = 2000;
        uint16_t poll_ms = 20;
        uint16_t battery_period_ms = 1500;
        uint32_t task_stack = 5120;
        UBaseType_t task_priority = 4;
        BaseType_t task_core = 1;
    };

    Power() = default;
    ~Power();
    Power(const Power&) = delete;
    Power& operator=(const Power&) = delete;

    esp_err_t begin(const Config& config);
    void end();
    State state() const;
    bool nextEvent(Event& event, TickType_t timeout = 0);
    bool buttonDown() const;
    bool waitForRelease(uint32_t timeout_ms) const;
    esp_err_t enterShipMode();
    void forceRefresh();

private:
    static void taskEntry(void* arg);
    void taskLoop();
    esp_err_t readRegs(uint8_t& reg08, uint8_t& reg09);
    esp_err_t readReg(uint8_t reg, uint8_t& value);
    esp_err_t writeReg(uint8_t reg, uint8_t value);
    bool sampleBattery(uint16_t& mv);
    static uint8_t batteryPercent(uint16_t mv);
    void publish(const State& state);

    Config config_{};
    i2c_master_dev_handle_t charger_ = nullptr;
    adc_oneshot_unit_handle_t adc_ = nullptr;
    adc_cali_handle_t cali_ = nullptr;
    bool cali_enabled_ = false;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    mutable SemaphoreHandle_t state_mutex_ = nullptr;
    mutable SemaphoreHandle_t i2c_mutex_ = nullptr;
    State state_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> refresh_requested_{false};
};

} // namespace pogopo::power

