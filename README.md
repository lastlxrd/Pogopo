# pogopoOS2.0 — ESP-IDF bring-up, Sharp driver step 1

This project is the ESP-IDF v6.0.2 hardware bring-up branch for the ESP32-S3 console.

Working baseline carried over:

- I2C on SDA GPIO8 / SCL GPIO9
- TCA9555 at 0x20
- BMI270 at 0x68
- BQ24295 at 0x6B
- TCA9555 button polling
- Flash/PSRAM settings in sdkconfig.defaults

New in this archive:

- `components/pogopo_sharp/`
- native ESP-IDF Sharp Memory LCD driver for LS027B7DH01
- PSRAM framebuffer, 400x240 1-bit
- internal DMA transfer buffer
- SPI2_HOST at 14 MHz, LSB-first protocol
- full refresh
- contiguous row partial refresh API
- dirty-row refresh API
- VCOM toggling on refresh / clear / vcom-only command
- basic drawing: pixel, lines, rects, fill_rect, tiny text

Build:

```powershell
idf.py set-target esp32s3
idf.py fullclean
idf.py build
idf.py -p COM7 flash monitor
```

Expected result:

- Serial still shows I2C scan and TCA/BMI/BQ tests.
- Sharp LCD should show a simple pogopoOS2.0 test screen.

If the screen is blank:

- confirm DISP is actually high on the board
- confirm EXTMODE is low
- confirm SCK GPIO12 / MOSI GPIO11 / CS GPIO14
- if needed, lower `cfg.clock_hz` in `main/app_main.cpp` from 14 MHz to 8 MHz


## FIX 2
Fixed Sharp init order: initialized_ is now set before the first refresh_full(), so refresh_rows() no longer returns ESP_ERR_INVALID_STATE during init.
