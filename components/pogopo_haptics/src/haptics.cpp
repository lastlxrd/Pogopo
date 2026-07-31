#include "pogopo/haptics/haptics.h"

#include "driver/gpio.h"
#include "esp_log.h"

namespace pogopo::haptics {
namespace {
constexpr char TAG[] = "pogopo_haptics";
}

Haptics::~Haptics() {
    end();
}

esp_err_t Haptics::begin(const Config& config) {
    if (ok_.load() || task_ || queue_) return ESP_ERR_INVALID_STATE;
    if (config.motor_io < 0 || config.queue_depth == 0) return ESP_ERR_INVALID_ARG;

    config_ = config;
    enabled_.store(true);

    gpio_config_t io_cfg = {};
    io_cfg.pin_bit_mask = 1ULL << config_.motor_io;
    io_cfg.mode = GPIO_MODE_OUTPUT;
    io_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    io_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_cfg.intr_type = GPIO_INTR_DISABLE;

    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) return err;
    gpio_ready_ = true;

    set_motor(false);

    queue_ = xQueueCreate(config_.queue_depth, sizeof(Command));
    if (!queue_) return ESP_ERR_NO_MEM;

    const BaseType_t created = xTaskCreatePinnedToCore(
        task_entry,
        "pogopo_haptics",
        config_.task_stack,
        this,
        config_.task_priority,
        &task_,
        config_.task_core);
    if (created != pdPASS) {
        vQueueDelete(queue_);
        queue_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ok_.store(true);
    ESP_LOGI(TAG, "Vibro ready GPIO%d active_%s",
             config_.motor_io, config_.active_high ? "HIGH" : "LOW");
    return ESP_OK;
}

void Haptics::end() {
    ok_.store(false);
    if (task_) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (queue_) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
    if (gpio_ready_) {
        set_motor(false);
        gpio_ready_ = false;
    }
}

bool Haptics::play(Effect effect) {
    if (!enabled_.load()) return true;
    Command command;
    command.effect = effect;
    return enqueue(command);
}

bool Haptics::pulse(uint16_t duration_ms) {
    if (!enabled_.load()) return true;
    if (duration_ms == 0) return false;
    Command command;
    command.custom_pulse = true;
    command.duration_ms = duration_ms;
    return enqueue(command);
}

void Haptics::clearPending() {
    if (queue_) xQueueReset(queue_);
}

void Haptics::setEnabled(bool enabled) {
    enabled_.store(enabled);
    if (!enabled) {
        clearPending();
        set_motor(false);
    }
}

bool Haptics::enqueue(const Command& command) {
    if (!ok_.load() || !queue_) return false;
    if (xQueueSend(queue_, &command, 0) != pdTRUE) {
        dropped_.fetch_add(1);
        return false;
    }
    return true;
}

void Haptics::task_entry(void* argument) {
    static_cast<Haptics*>(argument)->task_loop();
}

void Haptics::task_loop() {
    Command command;
    while (true) {
        if (xQueueReceive(queue_, &command, portMAX_DELAY) != pdTRUE) continue;

        if (command.custom_pulse) {
            const Segment segments[] = {{true, command.duration_ms}};
            run_segments(segments, 1);
        } else {
            run_effect(command.effect);
        }
        set_motor(false);
    }
}

void Haptics::run_effect(Effect effect) {
    switch (effect) {
        case Effect::Tick: {
            const Segment sequence[] = {{true, 55}};
            run_segments(sequence, 1);
            break;
        }
        case Effect::Click: {
            const Segment sequence[] = {{true, 64}};
            run_segments(sequence, 1);
            break;
        }
        case Effect::DoubleClick: {
            const Segment sequence[] = {{true, 36}, {false, 48}, {true, 36}};
            run_segments(sequence, 3);
            break;
        }
        case Effect::Confirm: {
            const Segment sequence[] = {{true, 30}, {false, 35}, {true, 76}};
            run_segments(sequence, 3);
            break;
        }
        case Effect::Alert: {
            const Segment sequence[] = {{true, 110}, {false, 65}, {true, 110}};
            run_segments(sequence, 3);
            break;
        }
        case Effect::Heavy: {
            const Segment sequence[] = {{true, 174}};
            run_segments(sequence, 1);
            break;
        }
    }
}

void Haptics::run_segments(const Segment* segments, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        set_motor(segments[i].on);
        if (segments[i].duration_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(segments[i].duration_ms));
        }
    }
}

void Haptics::set_motor(bool on) {
    if (!enabled_.load()) on = false;
    active_.store(on);
    if (!gpio_ready_) return;
    const int level = (on == config_.active_high) ? 1 : 0;
    gpio_set_level(static_cast<gpio_num_t>(config_.motor_io), level);
}

const char* effect_name(Effect effect) {
    switch (effect) {
        case Effect::Tick: return "TICK";
        case Effect::Click: return "CLICK";
        case Effect::DoubleClick: return "DOUBLE";
        case Effect::Confirm: return "CONFIRM";
        case Effect::Alert: return "ALERT";
        case Effect::Heavy: return "HEAVY";
        default: return "?";
    }
}

} // namespace pogopo::haptics
