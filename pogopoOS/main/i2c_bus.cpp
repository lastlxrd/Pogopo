#include "i2c_bus.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char* TAG = "i2c";
static i2c_master_bus_handle_t s_bus = nullptr;

i2c_master_bus_handle_t i2c_bus_handle() {
    return s_bus;
}

esp_err_t i2c_bus_init() {
    if (s_bus) return ESP_OK;

    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = static_cast<gpio_num_t>(board::I2C_SDA);
    cfg.scl_io_num = static_cast<gpio_num_t>(board::I2C_SCL);
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;

    const esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Initialized SDA=GPIO%d SCL=GPIO%d", board::I2C_SDA, board::I2C_SCL);
    } else {
        ESP_LOGE(TAG, "Init failed: %s", esp_err_to_name(err));
    }
    return err;
}

void i2c_scan() {
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "Device: 0x%02X", addr);
            ++found;
        }
    }
    ESP_LOGI(TAG, "Scan complete: %d device(s)", found);
}

static esp_err_t add_device(uint8_t address, i2c_master_dev_handle_t* dev) {
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = address;
    cfg.scl_speed_hz = 400000;
    return i2c_master_bus_add_device(s_bus, &cfg, dev);
}

esp_err_t i2c_read_reg(uint8_t address, uint8_t reg, uint8_t* data, size_t len) {
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    i2c_master_dev_handle_t dev = nullptr;
    esp_err_t err = add_device(address, &dev);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit_receive(dev, &reg, 1, data, len, 100);
    const esp_err_t rm = i2c_master_bus_rm_device(dev);
    return err != ESP_OK ? err : rm;
}

esp_err_t i2c_write_reg(uint8_t address, uint8_t reg, const uint8_t* data, size_t len) {
    if ((!data && len) || len > 16) return ESP_ERR_INVALID_ARG;

    uint8_t buffer[17] = {};
    buffer[0] = reg;
    for (size_t i = 0; i < len; ++i) buffer[i + 1] = data[i];

    i2c_master_dev_handle_t dev = nullptr;
    esp_err_t err = add_device(address, &dev);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit(dev, buffer, len + 1, 100);
    const esp_err_t rm = i2c_master_bus_rm_device(dev);
    return err != ESP_OK ? err : rm;
}

