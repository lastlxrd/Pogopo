# pogopoOS2.0 STEP7 — SD WAV + BMI270 + Power

This project is based on the fully tested previous step 6 build.

## Added in STEP7

### 1. `pogopo_storage`
- SDMMC 4-bit mode
- CLK GPIO6, CMD GPIO7
- D0 GPIO5, D1 GPIO4, D2 GPIO16, D3 GPIO15
- mount point: `/sdcard`
- scans `/sdcard/pogopo/sounds/*.wav`
- PCM WAV loader: 8/16-bit, mono/stereo, 8–96 kHz
- samples are converted to mono 16-bit and kept in PSRAM during playback

Copy this archive's `COPY_TO_SD/pogopo` folder to the root of your SD card.

### 2. PCM playback in `pogopo_audio`
- generated system sounds still use the 4-voice mixer
- WAV is mixed together with system sounds
- nearest-neighbour sample-rate conversion to the 32768 Hz I2S output
- WAV memory ownership is transferred to the audio task and released automatically
- UI Tick volume reduced from 48 to 32
- Error notes reduced from 64/68 to 49/52

### 3. `pogopo_imu`
- official Espressif BMI270 component
- address 0x68, chip ID 0x24
- accel ±2 g, gyro ±2000 dps, sensor ODR 100 Hz
- application samples every 20 ms
- normalized acceleration in g and gyro in dps
- calculated roll and pitch
- Motion Lab with live horizon and moving ball
- A calibrates the current pose as zero

The first build may download the managed component `espressif/bmi270`.

### 4. `pogopo_power`
Old pogopoOS1.0 shutdown behaviour was ported:
- Power button GPIO17, active LOW
- hold for 2 seconds
- if USB is connected, ship mode is blocked and a warning appears
- release the button to return to the OS
- without USB, release the button and BQ24295 BATFET ship mode is requested
- watchdog bits in REG05 are cleared before BATFET disable in REG07
- BQ24295 INT GPIO41
- battery divider GPIO1, NMOS gate GPIO2
- calibrated ADC one-shot reading, divider ×2

Power-on remains hardware-controlled by the charger/QON circuit, exactly as before.

## New launcher apps
- WAV Player
- Motion Lab
- Power Status

## Power test order
1. First open **Power Status** and verify battery voltage and USB state.
2. With USB connected, hold the physical power button for 2 seconds. It should show `USB CONNECTED` and must not switch off.
3. Release the button and return to the OS.
4. For the real shutdown test, disconnect USB, hold for 2 seconds, then release the button.
5. The BQ24295 should disable BATFET and the console should power off.

## Notes
- The SD mount is non-fatal. The OS still starts if the card is missing.
- The IMU init is non-fatal. Motion Lab shows an error if BMI270 is unavailable.
- Power/BQ initialization is treated as required because shutdown safety depends on it.
- GPIO18 is reserved for a future BMI270 data-ready interrupt mode; this step uses stable timed sampling.
