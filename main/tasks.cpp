#include "tasks.h"
#include "peripheral_tests.h"
#include "system_state.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "tasks";
SystemState g_system_state;

static void status_task(void*) {
    unsigned ticks = 0;
    while (true) {
        peripheral_poll();

        if (++ticks >= 10) {
            ticks = 0;
            const size_t internal =
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            const size_t psram =
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

            ESP_LOGI(TAG,
                     "RAM=%u PSRAM=%u | BMI=%d TCA=%d BQ=%d | buttons=0x%02X BQ08=0x%02X BQ09=0x%02X",
                     static_cast<unsigned>(internal),
                     static_cast<unsigned>(psram),
                     g_system_state.imu_ok.load(),
                     g_system_state.buttons_ok.load(),
                     g_system_state.charger_ok.load(),
                     g_system_state.buttons_port.load(),
                     g_system_state.charger_status.load(),
                     g_system_state.charger_fault.load());
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void start_system_tasks() {
    if (xTaskCreatePinnedToCore(status_task, "status_task", 4096,
                                nullptr, 2, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create status_task");
    }
}

