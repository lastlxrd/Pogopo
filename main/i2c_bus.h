#pragma once
#include <cstddef>
#include <cstdint>
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t i2c_bus_init();
i2c_master_bus_handle_t i2c_bus_handle();
void i2c_scan();
esp_err_t i2c_read_reg(uint8_t address, uint8_t reg, uint8_t* data, size_t len);
esp_err_t i2c_write_reg(uint8_t address, uint8_t reg, const uint8_t* data, size_t len);

