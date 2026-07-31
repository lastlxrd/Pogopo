#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "sdmmc_cmd.h"

namespace pogopo::storage {

struct FileEntry {
    char name[64]{};
    char path[160]{};
    uint32_t size = 0;
};

struct WavData {
    int16_t* samples = nullptr;      // mono signed PCM, allocated in PSRAM when possible
    uint32_t frames = 0;
    uint32_t sample_rate = 0;
    uint8_t source_channels = 0;
    uint8_t source_bits = 0;

    bool valid() const { return samples && frames && sample_rate; }
};

class Storage {
public:
    static constexpr size_t MAX_FILES = 12;

    struct Config {
        int clk_io = 6;
        int cmd_io = 7;
        int d0_io = 5;
        int d1_io = 4;
        int d2_io = 16;
        int d3_io = 15;
        const char* mount_point = "/sdcard";
        uint8_t max_files = 6;
        size_t allocation_unit_size = 16 * 1024;
    };

    Storage() = default;
    ~Storage();
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    esp_err_t begin(const Config& config);
    void end();
    bool mounted() const { return mounted_; }
    const char* mountPoint() const { return config_.mount_point; }
    uint64_t capacityBytes() const;

    size_t listWav(const char* relative_dir, FileEntry* out, size_t capacity) const;
    esp_err_t loadWav(const char* path, WavData& out) const;
    static void freeWav(WavData& wav);

private:
    Config config_{};
    sdmmc_card_t* card_ = nullptr;
    bool mounted_ = false;
};

} // namespace pogopo::storage
