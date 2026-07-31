#pragma once

#include <array>
#include <cstddef>
#include <atomic>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace pogopo::input {

enum class Button : uint8_t {
    None  = 0x00,
    Top   = 0x01, // TCA9555 P8  / port 1 bit 0
    Down  = 0x02, // P9
    Left  = 0x04, // P10
    Right = 0x08, // P11
    B     = 0x10, // P12
    A     = 0x20, // P13
    Menu  = 0x40, // P14
    Start = 0x80, // P15
};

using ButtonMask = uint8_t;

constexpr ButtonMask mask(Button button) {
    return static_cast<ButtonMask>(button);
}

constexpr ButtonMask operator|(Button a, Button b) {
    return static_cast<ButtonMask>(mask(a) | mask(b));
}

constexpr ButtonMask operator|(ButtonMask a, Button b) {
    return static_cast<ButtonMask>(a | mask(b));
}

enum class EventType : uint8_t {
    Pressed,
    Released,
    Repeat,
    LongPress,
};

struct Event {
    EventType type = EventType::Pressed;
    Button button = Button::None;
    ButtonMask held = 0;
    uint32_t timestamp_ms = 0;
    uint32_t held_ms = 0;
};

class Input {
public:
    struct Config {
        i2c_master_bus_handle_t bus = nullptr;
        uint8_t address = 0x20;
        int interrupt_io = 21;
        bool active_low = true;
        uint32_t i2c_clock_hz = 400000;
        uint32_t poll_period_ms = 4;
        uint8_t debounce_samples = 3;
        uint32_t repeat_delay_ms = 450;
        uint32_t repeat_period_ms = 100;
        uint32_t long_press_ms = 700;
        uint8_t queue_depth = 24;
        uint32_t task_stack = 4096;
        UBaseType_t task_priority = 5;
        BaseType_t task_core = 0;
    };

    Input() = default;
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    esp_err_t begin(const Config& config);
    void end();
    bool ok() const { return ok_.load(); }

    ButtonMask heldMask() const { return held_.load(); }
    ButtonMask rawPressedMask() const { return raw_pressed_.load(); }
    uint8_t rawPort() const { return raw_port_.load(); }

    bool held(Button button) const { return (heldMask() & mask(button)) != 0; }
    bool comboHeld(ButtonMask buttons) const {
        return buttons != 0 && (heldMask() & buttons) == buttons;
    }

    bool consumePressed(Button button);
    bool consumeReleased(Button button);
    bool consumeRepeat(Button button);
    bool consumeLongPress(Button button);

    ButtonMask consumePressedMask();
    ButtonMask consumeReleasedMask();
    ButtonMask consumeRepeatMask();
    ButtonMask consumeLongPressMask();

    bool nextEvent(Event& event, TickType_t wait_ticks = 0);
    uint32_t droppedEvents() const { return dropped_events_.load(); }
    uint32_t readErrors() const { return read_errors_.load(); }

private:
    static void task_entry(void* argument);
    static void gpio_isr(void* argument);
    void task_loop();
    esp_err_t read_port(uint8_t& port);
    esp_err_t configure_expander();
    void process_sample(uint8_t raw_port, uint32_t now_ms);
    void push_event(EventType type, Button button, uint32_t now_ms, uint32_t held_ms = 0);
    static bool consume_bit(std::atomic<uint8_t>& latch, Button button);
    static Button button_from_index(size_t index);

    Config config_{};
    i2c_master_dev_handle_t device_ = nullptr;
    QueueHandle_t event_queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    bool isr_added_ = false;

    std::atomic<bool> ok_{false};
    std::atomic<uint8_t> raw_port_{0xFF};
    std::atomic<uint8_t> raw_pressed_{0};
    std::atomic<uint8_t> held_{0};
    std::atomic<uint8_t> pressed_latch_{0};
    std::atomic<uint8_t> released_latch_{0};
    std::atomic<uint8_t> repeat_latch_{0};
    std::atomic<uint8_t> long_latch_{0};
    std::atomic<uint32_t> dropped_events_{0};
    std::atomic<uint32_t> read_errors_{0};

    std::array<uint8_t, 8> debounce_{};
    std::array<uint32_t, 8> pressed_at_ms_{};
    std::array<uint32_t, 8> repeat_at_ms_{};
    std::array<bool, 8> long_sent_{};
};

const char* button_name(Button button);
const char* event_type_name(EventType type);

} // namespace pogopo::input
