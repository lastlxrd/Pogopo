#include "pogopo/power/power.h"

#include <algorithm>

#include "driver/gpio.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_check.h"

namespace pogopo::power {
namespace { constexpr char TAG[] = "pogopo_power"; }

Power::~Power() { end(); }

esp_err_t Power::begin(const Config& config) {
    if (running_.load() || !config.bus) return ESP_ERR_INVALID_STATE;
    config_ = config;
    state_mutex_ = xSemaphoreCreateMutex();
    i2c_mutex_ = xSemaphoreCreateMutex();
    queue_ = xQueueCreate(4, sizeof(Event));
    if (!state_mutex_ || !i2c_mutex_ || !queue_) { end(); return ESP_ERR_NO_MEM; }

    gpio_config_t btn{};
    btn.pin_bit_mask = (1ULL << config.power_button_io) | (1ULL << config.charger_int_io);
    btn.mode = GPIO_MODE_INPUT; btn.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&btn), TAG, "button GPIO");
    gpio_config_t gate{};
    gate.pin_bit_mask = 1ULL << config.battery_gate_io;
    gate.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&gate), TAG, "gate GPIO");
    gpio_set_level(static_cast<gpio_num_t>(config.battery_gate_io), 0);

    i2c_device_config_t dev{};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = config.charger_address;
    dev.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(config.bus, &dev, &charger_), TAG, "BQ device");

    adc_oneshot_unit_init_cfg_t adc_init{};
    adc_init.unit_id = config.adc_unit;
    adc_init.ulp_mode = ADC_ULP_MODE_DISABLE;
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_init, &adc_), TAG, "ADC unit");
    adc_oneshot_chan_cfg_t channel{};
    channel.atten = ADC_ATTEN_DB_12;
    channel.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(adc_, config.adc_channel, &channel), TAG, "ADC channel");

    adc_cali_curve_fitting_config_t curve{};
    curve.unit_id = config.adc_unit;
    curve.chan = config.adc_channel;
    curve.atten = ADC_ATTEN_DB_12;
    curve.bitwidth = ADC_BITWIDTH_DEFAULT;
    cali_enabled_ = adc_cali_create_scheme_curve_fitting(&curve, &cali_) == ESP_OK;

    running_.store(true);
    if (xTaskCreatePinnedToCore(taskEntry, "pogopo_power", config.task_stack, this,
                                config.task_priority, &task_, config.task_core) != pdPASS) {
        running_.store(false); end(); return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Power ready: BTN GPIO%d hold=%ums, BQ=0x%02X, BAT ADC GPIO%d gate GPIO%d",
             config.power_button_io, config.shutdown_hold_ms, config.charger_address,
             config.battery_measure_io, config.battery_gate_io);
    return ESP_OK;
}

void Power::end() {
    running_.store(false);
    if (task_) { vTaskDelete(task_); task_ = nullptr; }
    if (cali_) { adc_cali_delete_scheme_curve_fitting(cali_); cali_ = nullptr; }
    if (adc_) { adc_oneshot_del_unit(adc_); adc_ = nullptr; }
    if (charger_) { i2c_master_bus_rm_device(charger_); charger_ = nullptr; }
    if (queue_) { vQueueDelete(queue_); queue_ = nullptr; }
    if (state_mutex_) { vSemaphoreDelete(state_mutex_); state_mutex_ = nullptr; }
    if (i2c_mutex_) { vSemaphoreDelete(i2c_mutex_); i2c_mutex_ = nullptr; }
    gpio_set_level(static_cast<gpio_num_t>(config_.battery_gate_io), 0);
}

State Power::state() const {
    State copy{};
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(5)) == pdTRUE) {
        copy = state_; xSemaphoreGive(state_mutex_);
    }
    return copy;
}

bool Power::nextEvent(Event& event, TickType_t timeout) {
    return queue_ && xQueueReceive(queue_, &event, timeout) == pdTRUE;
}
bool Power::buttonDown() const {
    return gpio_get_level(static_cast<gpio_num_t>(config_.power_button_io)) == 0;
}
bool Power::waitForRelease(uint32_t timeout_ms) const {
    const TickType_t start = xTaskGetTickCount();
    while (buttonDown()) {
        if (timeout_ms && (xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}
void Power::forceRefresh() { refresh_requested_.store(true); }

esp_err_t Power::readReg(uint8_t reg, uint8_t& value) {
    if (!charger_) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(i2c_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const esp_err_t err = i2c_master_transmit_receive(charger_, &reg, 1, &value, 1, 100);
    xSemaphoreGive(i2c_mutex_); return err;
}
esp_err_t Power::writeReg(uint8_t reg, uint8_t value) {
    if (!charger_) return ESP_ERR_INVALID_STATE;
    const uint8_t tx[2] = {reg, value};
    if (xSemaphoreTake(i2c_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const esp_err_t err = i2c_master_transmit(charger_, tx, sizeof(tx), 100);
    xSemaphoreGive(i2c_mutex_); return err;
}
esp_err_t Power::readRegs(uint8_t& r8, uint8_t& r9) {
    esp_err_t e = readReg(0x08, r8); if (e != ESP_OK) return e;
    return readReg(0x09, r9);
}

bool Power::sampleBattery(uint16_t& mv) {
    gpio_set_level(static_cast<gpio_num_t>(config_.battery_gate_io), 0);
    gpio_set_direction(static_cast<gpio_num_t>(config_.battery_measure_io), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(config_.battery_measure_io), 0);
    vTaskDelay(pdMS_TO_TICKS(15));
    gpio_set_direction(static_cast<gpio_num_t>(config_.battery_measure_io), GPIO_MODE_INPUT);
    gpio_set_level(static_cast<gpio_num_t>(config_.battery_gate_io), 1);
    vTaskDelay(pdMS_TO_TICKS(60));

    int raw = 0, dummy = 0;
    for (int i = 0; i < 12; ++i) adc_oneshot_read(adc_, config_.adc_channel, &dummy);
    int64_t sum = 0; int good = 0;
    for (int i = 0; i < 64; ++i) {
        if (adc_oneshot_read(adc_, config_.adc_channel, &raw) == ESP_OK) { sum += raw; ++good; }
    }
    gpio_set_level(static_cast<gpio_num_t>(config_.battery_gate_io), 0);
    gpio_set_direction(static_cast<gpio_num_t>(config_.battery_measure_io), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(config_.battery_measure_io), 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_direction(static_cast<gpio_num_t>(config_.battery_measure_io), GPIO_MODE_INPUT);
    if (!good) return false;

    const int average = static_cast<int>(sum / good);
    int adc_mv = 0;
    if (cali_enabled_) {
        if (adc_cali_raw_to_voltage(cali_, average, &adc_mv) != ESP_OK) return false;
    } else {
        adc_mv = (average * 3300) / 4095;
    }
    const int battery = adc_mv * 2;
    if (adc_mv < 250 || adc_mv > 2800 || battery < 2800 || battery > 4400) return false;
    mv = static_cast<uint16_t>(battery);
    return true;
}

uint8_t Power::batteryPercent(uint16_t mv) {
    if (mv >= 4200) return 100; if (mv >= 4100) return 90; if (mv >= 4000) return 80;
    if (mv >= 3900) return 65; if (mv >= 3800) return 50; if (mv >= 3700) return 35;
    if (mv >= 3600) return 20; if (mv >= 3500) return 10; if (mv >= 3400) return 5; return 0;
}

void Power::publish(const State& state) {
    if (xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        state_ = state; xSemaphoreGive(state_mutex_);
    }
}

void Power::taskEntry(void* arg) { static_cast<Power*>(arg)->taskLoop(); }
void Power::taskLoop() {
    TickType_t wake = xTaskGetTickCount();
    TickType_t pressed_at = 0, last_status = 0, last_battery = 0;
    bool event_sent = false;
    uint32_t sequence = 0, errors = 0;
    uint8_t invalid_battery_reads = 0;
    State s{};
    while (running_.load()) {
        const TickType_t now = xTaskGetTickCount();
        s.power_button_down = buttonDown();
        if (s.power_button_down) {
            if (!pressed_at) pressed_at = now;
            const uint32_t held = (now - pressed_at) * portTICK_PERIOD_MS;
            s.hold_percent = static_cast<uint8_t>(std::min<uint32_t>(100, held * 100 / config_.shutdown_hold_ms));
            if (!event_sent && held >= config_.shutdown_hold_ms) {
                uint8_t r8=0,r9=0;
                if (readRegs(r8,r9) == ESP_OK) { s.reg08=r8; s.reg09=r9; s.usb_present=(r8 & 0x04)!=0; }
                Event event{ s.usb_present ? EventType::UsbBlocked : EventType::ShutdownRequested };
                xQueueSend(queue_, &event, 0); event_sent = true;
            }
        } else { pressed_at = 0; event_sent = false; s.hold_percent = 0; }

        if (refresh_requested_.exchange(false) || now - last_status >= pdMS_TO_TICKS(500)) {
            last_status = now;
            uint8_t r8=0,r9=0;
            const esp_err_t e = readRegs(r8,r9);
            if (e == ESP_OK) {
                s.reg08=r8; s.reg09=r9; s.usb_present=(r8 & 0x04)!=0;
                s.charging=((r8 >> 4) & 0x03) == 1 || ((r8 >> 4) & 0x03) == 2;
                s.ok=true;
            } else { ++errors; s.ok=false; }
        }
        if (now - last_battery >= pdMS_TO_TICKS(config_.battery_period_ms)) {
            last_battery = now;
            uint16_t mv=0;
            if (sampleBattery(mv)) {
                s.battery_mv=mv; s.battery_percent=batteryPercent(mv); s.battery_valid=true; invalid_battery_reads=0;
            } else if (++invalid_battery_reads >= 5) s.battery_valid=false;
        }
        s.i2c_errors=errors; s.sequence=++sequence; publish(s);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(config_.poll_ms));
    }
    vTaskDelete(nullptr);
}

esp_err_t Power::enterShipMode() {
    uint8_t reg05=0, reg07=0;
    ESP_RETURN_ON_ERROR(readReg(0x05, reg05), TAG, "read REG05");
    reg05 &= static_cast<uint8_t>(~(0x03U << 4));
    ESP_RETURN_ON_ERROR(writeReg(0x05, reg05), TAG, "disable watchdog");
    ESP_RETURN_ON_ERROR(readReg(0x07, reg07), TAG, "read REG07");
    reg07 |= static_cast<uint8_t>(1U << 5);
    const esp_err_t err = writeReg(0x07, reg07);
    ESP_LOGI(TAG, "Ship mode command: %s REG05=0x%02X REG07=0x%02X", esp_err_to_name(err), reg05, reg07);
    return err;
}

} // namespace pogopo::power
