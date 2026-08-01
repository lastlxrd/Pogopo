#include "pogopo/storage/storage.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

namespace pogopo::storage {
namespace {
constexpr char TAG[] = "pogopo_storage";
constexpr uint32_t MAX_WAV_DATA = 4U * 1024U * 1024U;

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
char lower_ascii(char value) {
    return (value >= 'A' && value <= 'Z') ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool ends_extension(const char* name, const char* extension) {
    if (!name || !extension || !*extension) return true;
    const size_t name_length = std::strlen(name);
    const size_t extension_length = std::strlen(extension);
    if (name_length < extension_length) return false;
    const char* suffix = name + name_length - extension_length;
    for (size_t i = 0; i < extension_length; ++i) {
        if (lower_ascii(suffix[i]) != lower_ascii(extension[i])) return false;
    }
    return true;
}
}

Storage::~Storage() { end(); }

esp_err_t Storage::begin(const Config& config) {
    if (mounted_) return ESP_ERR_INVALID_STATE;
    if (!config.mount_point || config.clk_io < 0 || config.cmd_io < 0 || config.d0_io < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    config_ = config;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = static_cast<gpio_num_t>(config.clk_io);
    slot.cmd = static_cast<gpio_num_t>(config.cmd_io);
    slot.d0 = static_cast<gpio_num_t>(config.d0_io);
    slot.d1 = static_cast<gpio_num_t>(config.d1_io);
    slot.d2 = static_cast<gpio_num_t>(config.d2_io);
    slot.d3 = static_cast<gpio_num_t>(config.d3_io);
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files = config.max_files;
    mount.allocation_unit_size = config.allocation_unit_size;

    const esp_err_t err = esp_vfs_fat_sdmmc_mount(config.mount_point, &host, &slot, &mount, &card_);
    if (err != ESP_OK) {
        card_ = nullptr;
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return err;
    }
    mounted_ = true;
    ESP_LOGI(TAG, "SD mounted at %s, capacity %.1f MB", config.mount_point,
             static_cast<double>(capacityBytes()) / (1024.0 * 1024.0));
    return ESP_OK;
}

void Storage::end() {
    if (!mounted_) return;
    esp_vfs_fat_sdcard_unmount(config_.mount_point, card_);
    card_ = nullptr;
    mounted_ = false;
}

uint64_t Storage::capacityBytes() const {
    if (!card_) return 0;
    return static_cast<uint64_t>(card_->csd.capacity) * card_->csd.sector_size;
}

size_t Storage::listFiles(const char* relative_dir, const char* extension,
                          FileEntry* out, size_t capacity) const {
    if (!mounted_ || !out || capacity == 0) return 0;

    char dir_path[192]{};
    const char* mount = config_.mount_point;
    const char* relative = relative_dir ? relative_dir : "";
    const size_t mount_len = std::strlen(mount);
    const size_t relative_len = std::strlen(relative);
    const bool add_slash = relative_len > 0 && relative[0] != '/';
    const size_t dir_len = mount_len + (add_slash ? 1U : 0U) + relative_len;
    if (dir_len >= sizeof(dir_path)) return 0;

    std::memcpy(dir_path, mount, mount_len);
    size_t offset = mount_len;
    if (add_slash) dir_path[offset++] = '/';
    if (relative_len) std::memcpy(dir_path + offset, relative, relative_len);
    dir_path[dir_len] = '\0';

    DIR* dir = opendir(dir_path);
    if (!dir) return 0;

    size_t count = 0;
    while (count < capacity) {
        dirent* ent = readdir(dir);
        if (!ent) break;
        if (!ends_extension(ent->d_name, extension)) continue;

        const size_t name_len = std::strlen(ent->d_name);
        const size_t path_len = dir_len + 1U + name_len;
        if (name_len >= sizeof(FileEntry::name) || path_len >= sizeof(FileEntry::path)) {
            ESP_LOGW(TAG, "Skipping file with path that is too long: %s", ent->d_name);
            continue;
        }

        FileEntry& item = out[count];
        item = {};
        std::memcpy(item.name, ent->d_name, name_len + 1U);
        std::memcpy(item.path, dir_path, dir_len);
        item.path[dir_len] = '/';
        std::memcpy(item.path + dir_len + 1U, ent->d_name, name_len + 1U);

        struct stat st{};
        if (stat(item.path, &st) == 0) item.size = static_cast<uint32_t>(st.st_size);
        ++count;
    }
    closedir(dir);

    std::sort(out, out + count, [](const FileEntry& a, const FileEntry& b) {
        return std::strcmp(a.name, b.name) < 0;
    });
    return count;
}

size_t Storage::listWav(const char* relative_dir, FileEntry* out, size_t capacity) const {
    return listFiles(relative_dir, ".wav", out, capacity);
}

esp_err_t Storage::loadWav(const char* path, WavData& out) const {
    freeWav(out);
    if (!mounted_ || !path) return ESP_ERR_INVALID_STATE;
    FILE* f = std::fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    uint8_t riff[12]{};
    if (std::fread(riff, 1, sizeof(riff), f) != sizeof(riff) ||
        std::memcmp(riff, "RIFF", 4) || std::memcmp(riff + 8, "WAVE", 4)) {
        std::fclose(f); return ESP_ERR_INVALID_RESPONSE;
    }

    uint16_t format = 0, channels = 0, bits = 0, block_align = 0;
    uint32_t sample_rate = 0, data_size = 0;
    long data_offset = 0;
    while (!data_offset) {
        uint8_t chunk[8]{};
        if (std::fread(chunk, 1, 8, f) != 8) break;
        const uint32_t size = le32(chunk + 4);
        if (!std::memcmp(chunk, "fmt ", 4)) {
            if (size < 16 || size > 256) { std::fclose(f); return ESP_ERR_NOT_SUPPORTED; }
            uint8_t fmt[256]{};
            if (std::fread(fmt, 1, size, f) != size) { std::fclose(f); return ESP_ERR_INVALID_SIZE; }
            format = le16(fmt); channels = le16(fmt + 2); sample_rate = le32(fmt + 4);
            block_align = le16(fmt + 12); bits = le16(fmt + 14);
            if (size & 1U) std::fseek(f, 1, SEEK_CUR);
        } else if (!std::memcmp(chunk, "data", 4)) {
            data_offset = std::ftell(f); data_size = size;
            std::fseek(f, static_cast<long>(size + (size & 1U)), SEEK_CUR);
        } else {
            std::fseek(f, static_cast<long>(size + (size & 1U)), SEEK_CUR);
        }
    }

    if (format != 1 || (channels != 1 && channels != 2) ||
        (bits != 8 && bits != 16) || sample_rate < 8000 || sample_rate > 96000 ||
        block_align == 0 || block_align > 4 || block_align != channels * (bits / 8) ||
        data_size == 0 || data_size > MAX_WAV_DATA) {
        std::fclose(f); return ESP_ERR_NOT_SUPPORTED;
    }
    const uint32_t frames = data_size / block_align;
    if (!frames) { std::fclose(f); return ESP_ERR_INVALID_SIZE; }

    int16_t* samples = static_cast<int16_t*>(heap_caps_malloc(
        static_cast<size_t>(frames) * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!samples) samples = static_cast<int16_t*>(heap_caps_malloc(
        static_cast<size_t>(frames) * sizeof(int16_t), MALLOC_CAP_8BIT));
    if (!samples) { std::fclose(f); return ESP_ERR_NO_MEM; }

    std::fseek(f, data_offset, SEEK_SET);
    uint8_t frame[4]{};
    uint32_t loaded = 0;
    for (; loaded < frames; ++loaded) {
        if (std::fread(frame, 1, block_align, f) != block_align) break;
        int32_t mixed = 0;
        if (bits == 8) {
            mixed = (static_cast<int32_t>(frame[0]) - 128) << 8;
            if (channels == 2) mixed = (mixed + ((static_cast<int32_t>(frame[1]) - 128) << 8)) / 2;
        } else {
            const int16_t left = static_cast<int16_t>(le16(frame));
            mixed = left;
            if (channels == 2) {
                const int16_t right = static_cast<int16_t>(le16(frame + 2));
                mixed = (static_cast<int32_t>(left) + right) / 2;
            }
        }
        samples[loaded] = static_cast<int16_t>(mixed);
    }
    std::fclose(f);
    if (!loaded) { heap_caps_free(samples); return ESP_ERR_INVALID_SIZE; }

    out.samples = samples; out.frames = loaded; out.sample_rate = sample_rate;
    out.source_channels = static_cast<uint8_t>(channels); out.source_bits = static_cast<uint8_t>(bits);
    ESP_LOGI(TAG, "WAV loaded: %s, %lu frames @ %lu Hz, %uch/%ubit", path,
             static_cast<unsigned long>(loaded), static_cast<unsigned long>(sample_rate), channels, bits);
    return ESP_OK;
}

void Storage::freeWav(WavData& wav) {
    if (wav.samples) heap_caps_free(wav.samples);
    wav = {};
}

} // namespace pogopo::storage

