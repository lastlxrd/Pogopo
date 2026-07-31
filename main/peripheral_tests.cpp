#include "peripheral_tests.h"
#include "i2c_bus.h"
#include "system_state.h"
#include "esp_log.h"

static const char* TAG = "periph";
static constexpr uint8_t TCA9555_ADDR = 0x20;
static constexpr uint8_t BMI270_ADDR  = 0x68;
static constexpr uint8_t BQ24295_ADDR = 0x6B;

static bool read8(uint8_t addr, uint8_t reg, uint8_t& value) {
    const esp_err_t err = i2c_read_reg(addr, reg, &value, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Read failed addr=0x%02X reg=0x%02X: %s",
                 addr, reg, esp_err_to_name(err));
        return false;
    }
    return true;
}

static void test_bmi270() {
    uint8_t chip_id = 0, err_reg = 0, status = 0;
    const bool ok = read8(BMI270_ADDR, 0x00, chip_id) &&
                    read8(BMI270_ADDR, 0x02, err_reg) &&
                    read8(BMI270_ADDR, 0x03, status);

    g_system_state.imu_ok = ok && chip_id == 0x24;
    ESP_LOGI(TAG,
             "BMI270 @0x68: CHIP_ID=0x%02X expected=0x24 ERR=0x%02X STATUS=0x%02X => %s",
             chip_id, err_reg, status,
             g_system_state.imu_ok.load() ? "OK" : "FAIL");
}

static void test_tca9555() {
    uint8_t in0=0, in1=0, out0=0, out1=0, pol0=0, pol1=0, cfg0=0, cfg1=0;

    bool ok = read8(TCA9555_ADDR, 0x00, in0) &&
              read8(TCA9555_ADDR, 0x01, in1) &&
              read8(TCA9555_ADDR, 0x02, out0) &&
              read8(TCA9555_ADDR, 0x03, out1) &&
              read8(TCA9555_ADDR, 0x04, pol0) &&
              read8(TCA9555_ADDR, 0x05, pol1) &&
              read8(TCA9555_ADDR, 0x06, cfg0) &&
              read8(TCA9555_ADDR, 0x07, cfg1);

    const uint8_t all_inputs = 0xFF;
    if (ok) {
        ok = i2c_write_reg(TCA9555_ADDR, 0x07, &all_inputs, 1) == ESP_OK;
        if (ok) read8(TCA9555_ADDR, 0x07, cfg1);
    }

    g_system_state.buttons_ok = ok;
    g_system_state.buttons_port = in1;

    ESP_LOGI(TAG,
             "TCA9555 @0x20: IN=%02X/%02X OUT=%02X/%02X POL=%02X/%02X CFG=%02X/%02X => %s",
             in0, in1, out0, out1, pol0, pol1, cfg0, cfg1, ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "Buttons P8..P15 raw=0x%02X", in1);
}

static const char* charge_state_name(uint8_t reg08) {
    switch ((reg08 >> 4) & 0x03) {
        case 0: return "not charging";
        case 1: return "pre-charge";
        case 2: return "fast charge";
        case 3: return "charge done";
    }
    return "?";
}

static void test_bq24295() {
    uint8_t r[11] = {};
    bool ok = true;

    for (uint8_t reg = 0; reg <= 0x0A; ++reg) {
        if (!read8(BQ24295_ADDR, reg, r[reg])) {
            ok = false;
            break;
        }
    }

    const uint8_t pn = (r[0x0A] >> 5) & 0x07;
    const uint8_t rev = r[0x0A] & 0x07;
    const bool identified = ok && pn == 0x06;

    g_system_state.charger_ok = identified;
    g_system_state.charger_status = r[0x08];
    g_system_state.charger_fault = r[0x09];

    if (!ok) {
        ESP_LOGE(TAG, "BQ24295 register read failed");
        return;
    }

    ESP_LOGI(TAG, "BQ24295 @0x6B: REG0A=0x%02X PN=%u expected=6 REV=%u => %s",
             r[0x0A], pn, rev, identified ? "OK" : "UNKNOWN");
    ESP_LOGI(TAG, "BQ status: REG08=0x%02X [%s PG=%u DPM=%u] REG09=0x%02X",
             r[0x08], charge_state_name(r[0x08]),
             (r[0x08] >> 2) & 1, (r[0x08] >> 3) & 1, r[0x09]);
    ESP_LOGI(TAG, "BQ regs: 00=%02X 01=%02X 02=%02X 03=%02X 04=%02X 05=%02X",
             r[0],r[1],r[2],r[3],r[4],r[5]);
    ESP_LOGI(TAG, "BQ regs: 06=%02X 07=%02X 08=%02X 09=%02X 0A=%02X",
             r[6],r[7],r[8],r[9],r[10]);
}

void run_peripheral_tests() {
    ESP_LOGI(TAG, "========== PERIPHERAL TEST ==========");
    test_tca9555();
    test_bmi270();
    test_bq24295();
    ESP_LOGI(TAG, "=====================================");
}

void peripheral_poll() {
    // TCA9555 button polling is owned by the pogopo_input component from STEP4.
    // Keep this background poll focused on charger state so two independent
    // tasks do not repeatedly read the same input register.
    uint8_t status = 0, fault = 0;
    if (read8(BQ24295_ADDR, 0x08, status) &&
        read8(BQ24295_ADDR, 0x09, fault)) {
        const uint8_t prev_s = g_system_state.charger_status.exchange(status);
        const uint8_t prev_f = g_system_state.charger_fault.exchange(fault);
        g_system_state.charger_ok = true;
        if (status != prev_s || fault != prev_f) {
            ESP_LOGI(TAG, "BQ CHANGE: REG08 %02X->%02X REG09 %02X->%02X",
                     prev_s, status, prev_f, fault);
        }
    }
}
