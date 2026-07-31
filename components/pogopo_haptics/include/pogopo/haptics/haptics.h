#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace pogopo::haptics {

enum class Effect : uint8_t {
    Tick,
    Click,
    DoubleClick,
    Confirm,
    Alert,
    Heavy,
};

class Haptics {
public:
    struct Config {
        int motor_io = 3;
        bool active_high = true;
        uint8_t queue_depth = 8;
        uint32_t task_stack = 3072;
        UBaseType_t task_priority = 4;
        BaseType_t task_core = 0;
    };

    Haptics() = default;
    ~Haptics();
    Haptics(const Haptics&) = delete;
    Haptics& operator=(const Haptics&) = delete;

    esp_err_t begin(const Config& config);
    void end();

    bool play(Effect effect);
    bool pulse(uint16_t duration_ms);
    void clearPending();

    bool ok() const { return ok_.load(); }
    bool active() const { return active_.load(); }
    uint32_t droppedCommands() const { return dropped_.load(); }

private:
    struct Command {
        bool custom_pulse = false;
        Effect effect = Effect::Click;
        uint16_t duration_ms = 0;
    };

    struct Segment {
        bool on;
        uint16_t duration_ms;
    };

    static void task_entry(void* argument);
    void task_loop();
    void run_effect(Effect effect);
    void run_segments(const Segment* segments, size_t count);
    void set_motor(bool on);
    bool enqueue(const Command& command);

    Config config_{};
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    bool gpio_ready_ = false;
    std::atomic<bool> ok_{false};
    std::atomic<bool> active_{false};
    std::atomic<uint32_t> dropped_{0};
};

const char* effect_name(Effect effect);

} // namespace pogopo::haptics
