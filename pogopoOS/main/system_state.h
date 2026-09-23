#pragma once
#include <atomic>
#include <cstdint>

struct SystemState {
    std::atomic_bool imu_ok{false};
    std::atomic_bool buttons_ok{false};
    std::atomic_bool charger_ok{false};
    std::atomic_uint8_t buttons_port{0xFF};
    std::atomic_uint8_t charger_status{0};
    std::atomic_uint8_t charger_fault{0};
};
extern SystemState g_system_state;

