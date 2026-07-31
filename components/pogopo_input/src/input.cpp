#include "pogopo/input/input.h"

#include <algorithm>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace pogopo::input {
namespace {
constexpr uint8_t REG_INPUT_PORT1 = 0x01;
constexpr uint8_t REG_CONFIG_PORT1 = 0x07;
constexpr char TAG[] = "pogopo_input";

uint32_t now_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}
} // namespace

Input::~Input() {
    end();
}

esp_err_t Input::begin(const Config& config) {
    if (ok_.load() || task_ || device_) return ESP_ERR_INVALID_STATE;
    if (!config.bus || config.queue_depth == 0 || config.debounce_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    config_ = config;

    i2c_device_config_t device_config = {};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = config_.address;
    device_config.scl_speed_hz = config_.i2c_clock_hz;

    esp_err_t err = i2c_master_bus_add_device(config_.bus, &device_config, &device_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not add TCA9555 device: %s", esp_err_to_name(err));
        return err;
    }

    err = configure_expander();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure TCA9555: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        return err;
    }

    uint8_t initial_port = 0xFF;
    if (read_port(initial_port) == ESP_OK) {
        raw_port_.store(initial_port);
        const uint8_t pressed = config_.active_low
            ? static_cast<uint8_t>(~initial_port)
            : initial_port;
        raw_pressed_.store(pressed);
    }

    event_queue_ = xQueueCreate(config_.queue_depth, sizeof(Event));
    if (!event_queue_) {
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        task_entry,
        "pogopo_input",
        config_.task_stack,
        this,
        config_.task_priority,
        &task_,
        config_.task_core);
    if (created != pdPASS) {
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    if (config_.interrupt_io >= 0) {
        gpio_config_t io_cfg = {};
        io_cfg.pin_bit_mask = 1ULL << config_.interrupt_io;
        io_cfg.mode = GPIO_MODE_INPUT;
        io_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        io_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_cfg.intr_type = GPIO_INTR_NEGEDGE;

        err = gpio_config(&io_cfg);
        if (err == ESP_OK) {
            const esp_err_t service = gpio_install_isr_service(0);
            if (service == ESP_OK || service == ESP_ERR_INVALID_STATE) {
                err = gpio_isr_handler_add(
                    static_cast<gpio_num_t>(config_.interrupt_io), gpio_isr, this);
                isr_added_ = (err == ESP_OK);
            } else {
                err = service;
            }
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GPIO%d INT unavailable, polling fallback: %s",
                     config_.interrupt_io, esp_err_to_name(err));
        }
    }

    ok_.store(true);
    ESP_LOGI(TAG,
             "TCA9555 input ready addr=0x%02X INT=GPIO%d poll=%ums debounce=%u samples",
             config_.address, config_.interrupt_io,
             static_cast<unsigned>(config_.poll_period_ms),
             static_cast<unsigned>(config_.debounce_samples));
    return ESP_OK;
}

void Input::end() {
    ok_.store(false);

    if (isr_added_ && config_.interrupt_io >= 0) {
        gpio_isr_handler_remove(static_cast<gpio_num_t>(config_.interrupt_io));
        isr_added_ = false;
    }

    if (task_) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (event_queue_) {
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
    }
    if (device_) {
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
    }

    held_.store(0);
    pressed_latch_.store(0);
    released_latch_.store(0);
    repeat_latch_.store(0);
    long_latch_.store(0);
}

esp_err_t Input::configure_expander() {
    const uint8_t value = 0xFF;
    const uint8_t payload[2] = {REG_CONFIG_PORT1, value};
    return i2c_master_transmit(device_, payload, sizeof(payload), 100);
}

esp_err_t Input::read_port(uint8_t& port) {
    const uint8_t reg = REG_INPUT_PORT1;
    return i2c_master_transmit_receive(device_, &reg, 1, &port, 1, 10);
}

void Input::task_entry(void* argument) {
    static_cast<Input*>(argument)->task_loop();
}

void Input::gpio_isr(void* argument) {
    auto* self = static_cast<Input*>(argument);
    if (!self || !self->task_) return;
    BaseType_t higher_priority_woken = pdFALSE;
    vTaskNotifyGiveFromISR(self->task_, &higher_priority_woken);
    if (higher_priority_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void Input::task_loop() {
    while (true) {
        if (config_.interrupt_io >= 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(config_.poll_period_ms));
        } else {
            vTaskDelay(pdMS_TO_TICKS(config_.poll_period_ms));
        }

        uint8_t port = 0xFF;
        const esp_err_t err = read_port(port);
        if (err != ESP_OK) {
            read_errors_.fetch_add(1);
            ok_.store(false);
            // Never preserve a stale held direction forever. Feeding a released
            // sample through the normal debounce path clears a stuck key after
            // three failed reads, while one transient I2C error changes nothing.
            process_sample(0xFF, now_ms());
            continue;
        }

        ok_.store(true);
        process_sample(port, now_ms());
    }
}

void Input::process_sample(uint8_t raw_port, uint32_t time_ms) {
    raw_port_.store(raw_port);
    const uint8_t raw_pressed = config_.active_low
        ? static_cast<uint8_t>(~raw_port)
        : raw_port;
    raw_pressed_.store(raw_pressed);

    uint8_t stable = held_.load();

    for (size_t index = 0; index < 8; ++index) {
        const uint8_t bit = static_cast<uint8_t>(1U << index);
        const bool raw_is_pressed = (raw_pressed & bit) != 0;

        if (raw_is_pressed) {
            debounce_[index] = std::min<uint8_t>(
                config_.debounce_samples,
                static_cast<uint8_t>(debounce_[index] + 1));
        } else if (debounce_[index] > 0) {
            --debounce_[index];
        }

        const bool was_pressed = (stable & bit) != 0;
        if (!was_pressed && debounce_[index] >= config_.debounce_samples) {
            stable = static_cast<uint8_t>(stable | bit);
            pressed_at_ms_[index] = time_ms;
            repeat_at_ms_[index] = time_ms + config_.repeat_delay_ms;
            long_sent_[index] = false;
            pressed_latch_.fetch_or(bit);
            held_.store(stable);
            push_event(EventType::Pressed, button_from_index(index), time_ms);
        } else if (was_pressed && debounce_[index] == 0) {
            stable = static_cast<uint8_t>(stable & static_cast<uint8_t>(~bit));
            const uint32_t duration = time_ms - pressed_at_ms_[index];
            released_latch_.fetch_or(bit);
            held_.store(stable);
            push_event(EventType::Released, button_from_index(index), time_ms, duration);
            long_sent_[index] = false;
        }
    }

    held_.store(stable);

    for (size_t index = 0; index < 8; ++index) {
        const uint8_t bit = static_cast<uint8_t>(1U << index);
        if ((stable & bit) == 0) continue;

        const uint32_t duration = time_ms - pressed_at_ms_[index];
        if (!long_sent_[index] && duration >= config_.long_press_ms) {
            long_sent_[index] = true;
            long_latch_.fetch_or(bit);
            push_event(EventType::LongPress, button_from_index(index), time_ms, duration);
        }

        if (config_.repeat_period_ms > 0 &&
            static_cast<int32_t>(time_ms - repeat_at_ms_[index]) >= 0) {
            repeat_at_ms_[index] = time_ms + config_.repeat_period_ms;
            repeat_latch_.fetch_or(bit);
            push_event(EventType::Repeat, button_from_index(index), time_ms, duration);
        }
    }
}

void Input::push_event(EventType type, Button button, uint32_t time_ms, uint32_t held_ms) {
    if (!event_queue_) return;
    Event event;
    event.type = type;
    event.button = button;
    event.held = held_.load();
    if (type == EventType::Pressed) {
        event.held = static_cast<uint8_t>(event.held | mask(button));
    }
    event.timestamp_ms = time_ms;
    event.held_ms = held_ms;

    if (xQueueSend(event_queue_, &event, 0) != pdTRUE) {
        dropped_events_.fetch_add(1);
    }
}

bool Input::consume_bit(std::atomic<uint8_t>& latch, Button button) {
    const uint8_t bit = mask(button);
    const uint8_t old = latch.fetch_and(static_cast<uint8_t>(~bit));
    return (old & bit) != 0;
}

bool Input::consumePressed(Button button) { return consume_bit(pressed_latch_, button); }
bool Input::consumeReleased(Button button) { return consume_bit(released_latch_, button); }
bool Input::consumeRepeat(Button button) { return consume_bit(repeat_latch_, button); }
bool Input::consumeLongPress(Button button) { return consume_bit(long_latch_, button); }

ButtonMask Input::consumePressedMask() { return pressed_latch_.exchange(0); }
ButtonMask Input::consumeReleasedMask() { return released_latch_.exchange(0); }
ButtonMask Input::consumeRepeatMask() { return repeat_latch_.exchange(0); }
ButtonMask Input::consumeLongPressMask() { return long_latch_.exchange(0); }

bool Input::nextEvent(Event& event, TickType_t wait_ticks) {
    return event_queue_ && xQueueReceive(event_queue_, &event, wait_ticks) == pdTRUE;
}

Button Input::button_from_index(size_t index) {
    return static_cast<Button>(static_cast<uint8_t>(1U << index));
}

const char* button_name(Button button) {
    switch (button) {
        case Button::Top: return "TOP";
        case Button::Down: return "DOWN";
        case Button::Left: return "LEFT";
        case Button::Right: return "RIGHT";
        case Button::B: return "B";
        case Button::A: return "A";
        case Button::Menu: return "MENU";
        case Button::Start: return "START";
        default: return "NONE";
    }
}

const char* event_type_name(EventType type) {
    switch (type) {
        case EventType::Pressed: return "PRESS";
        case EventType::Released: return "RELEASE";
        case EventType::Repeat: return "REPEAT";
        case EventType::LongPress: return "LONG";
        default: return "?";
    }
}

} // namespace pogopo::input
