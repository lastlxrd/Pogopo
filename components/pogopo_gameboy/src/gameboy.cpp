#include "pogopo/gameboy/gameboy.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

#define ENABLE_LCD 1
#define ENABLE_SOUND 1
#define PEANUT_GB_12_COLOUR 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 1

extern "C" {
#include "minigb_apu.h"
uint8_t audio_read(uint16_t address);
void audio_write(uint16_t address, uint8_t value);
#include "peanut_gb.h"
}

namespace {
constexpr char TAG[] = "pogopo_gb";
constexpr uint32_t ROM_CACHE_PAGE_SIZE = 16U * 1024U;
constexpr uint8_t MAX_CACHE_PAGES = 8;
constexpr int64_t FRAME_PERIOD_US = 16743; // 59.7275 Hz DMG, rounded to microseconds.
constexpr uint32_t MAX_SAVE_BYTES = 128U * 1024U;
// MiniGB emits AUDIO_SAMPLES once per 59.7275 Hz DMG frame. That producer
// clock is ~32730.7 stereo frames/s, slightly below the nominal 32768 Hz APU
// compile-time rate. Feeding the real producer rate to pogopo_audio prevents
// a periodic underrun without changing the hardware I2S rate.
constexpr uint32_t GB_AUDIO_SOURCE_RATE = 32730;

std::atomic<pogopo::gameboy::GameBoy*> g_active_gameboy{nullptr};

uint8_t* allocate_bytes(size_t size, uint32_t preferred_caps, uint32_t fallback_caps) {
    auto* memory = static_cast<uint8_t*>(heap_caps_malloc(size, preferred_caps));
    if (!memory && fallback_caps != preferred_caps) {
        memory = static_cast<uint8_t*>(heap_caps_malloc(size, fallback_caps));
    }
    return memory;
}

} // namespace

extern "C" uint8_t audio_read(uint16_t address) {
    auto* gameboy = g_active_gameboy.load(std::memory_order_acquire);
    return gameboy ? gameboy->apuRead(address) : 0xFF;
}

extern "C" void audio_write(uint16_t address, uint8_t value) {
    auto* gameboy = g_active_gameboy.load(std::memory_order_acquire);
    if (gameboy) gameboy->apuWrite(address, value);
}

namespace pogopo::gameboy {

struct GameBoy::Impl {
    gb_s core{};
    minigb_apu_ctx apu{};

    uint8_t* rom_data = nullptr;
    uint32_t rom_size = 0;
    bool rom_in_psram = false;

    uint8_t* rom_cache = nullptr;
    uint8_t rom_cache_pages = 0;
    uint32_t cache_page_index[MAX_CACHE_PAGES]{};
    uint32_t cache_age[MAX_CACHE_PAGES]{};
    uint32_t cache_tick = 0;
    uint32_t cache_last_page = UINT32_MAX;
    uint8_t* cache_last_pointer = nullptr;

    uint8_t* save_data = nullptr;
    uint32_t save_size = 0;
    bool save_dirty = false;
    int64_t last_save_us = 0;

    uint8_t* frame_front = nullptr;
    uint8_t* frame_back = nullptr;
    uint8_t* frame_render = nullptr;
    bool previous_valid = false;
    uint8_t lcd_publish_counter = 0;
    ScaleMode previous_scale = ScaleMode::FitHeight;
    uint32_t last_draw_sequence = UINT32_MAX;

    SemaphoreHandle_t frame_mutex = nullptr;
    SemaphoreHandle_t core_mutex = nullptr;

    audio_sample_t audio_samples[AUDIO_SAMPLES_TOTAL]{};

    char rom_path[192]{};
    char save_path[192]{};
    char rom_title[32]{};

    std::atomic<uint32_t> emulated_frames{0};
    std::atomic<uint32_t> displayed_frames{0};
    std::atomic<uint32_t> audio_frames_pushed{0};
    std::atomic<uint32_t> audio_frames_dropped{0};
    std::atomic<uint32_t> cache_hits{0};
    std::atomic<uint32_t> cache_misses{0};
    std::atomic<uint32_t> save_writes{0};
    std::atomic<uint32_t> last_frame_us{0};
    std::atomic<uint32_t> max_frame_us{0};
};

GameBoy::~GameBoy() {
    end();
}

esp_err_t GameBoy::begin(audio::Audio& audio) {
    return begin(audio, Config{});
}

esp_err_t GameBoy::begin(audio::Audio& audio, const Config& config) {
    if (initialized_.load() || impl_) return ESP_ERR_INVALID_STATE;
    if (!audio.ok() || config.requested_cache_pages == 0 ||
        config.requested_cache_pages > MAX_CACHE_PAGES || config.task_stack < 4096 ||
        config.display_divider == 0 || config.display_divider > 4) {
        return ESP_ERR_INVALID_ARG;
    }

    void* storage = heap_caps_malloc(sizeof(Impl), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!storage) return ESP_ERR_NO_MEM;
    impl_ = new (storage) Impl{};
    config_ = config;
    audio_ = &audio;

    impl_->frame_mutex = xSemaphoreCreateMutex();
    impl_->core_mutex = xSemaphoreCreateMutex();
    if (!impl_->frame_mutex || !impl_->core_mutex) {
        end();
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t frame_error = allocateFrames();
    if (frame_error != ESP_OK) {
        end();
        return frame_error;
    }

    for (uint8_t i = 0; i < MAX_CACHE_PAGES; ++i) {
        impl_->cache_page_index[i] = UINT32_MAX;
    }

    initialized_.store(true);
    last_error_.store(ESP_OK);
    ESP_LOGI(TAG, "Game Boy frontend ready: Peanut-GB + MiniGB APU");
    return ESP_OK;
}

void GameBoy::end() {
    unload();
    initialized_.store(false);
    audio_ = nullptr;
    g_active_gameboy.store(nullptr, std::memory_order_release);

    if (!impl_) return;
    freeFrames();
    if (impl_->frame_mutex) {
        vSemaphoreDelete(impl_->frame_mutex);
        impl_->frame_mutex = nullptr;
    }
    if (impl_->core_mutex) {
        vSemaphoreDelete(impl_->core_mutex);
        impl_->core_mutex = nullptr;
    }
    impl_->~Impl();
    heap_caps_free(impl_);
    impl_ = nullptr;
}

esp_err_t GameBoy::allocateFrames() {
    if (!impl_) return ESP_ERR_INVALID_STATE;
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    // These buffers are touched for every LCD line and every displayed frame.
    // Prefer internal RAM; PSRAM is only a fallback.
    impl_->frame_front = allocate_bytes(FRAME_PIXELS, internal, psram);
    impl_->frame_back = allocate_bytes(FRAME_PIXELS, internal, psram);
    impl_->frame_render = allocate_bytes(FRAME_PIXELS, internal, psram);
    if (!impl_->frame_front || !impl_->frame_back || !impl_->frame_render) {
        freeFrames();
        return ESP_ERR_NO_MEM;
    }
    std::memset(impl_->frame_front, 0, FRAME_PIXELS);
    std::memset(impl_->frame_back, 0, FRAME_PIXELS);
    std::memset(impl_->frame_render, 0, FRAME_PIXELS);
    return ESP_OK;
}

void GameBoy::freeFrames() {
    if (!impl_) return;
    if (impl_->frame_front) heap_caps_free(impl_->frame_front);
    if (impl_->frame_back) heap_caps_free(impl_->frame_back);
    if (impl_->frame_render) heap_caps_free(impl_->frame_render);
    impl_->frame_front = nullptr;
    impl_->frame_back = nullptr;
    impl_->frame_render = nullptr;
}

esp_err_t GameBoy::load(const char* path) {
    if (!initialized_.load() || !impl_ || !audio_) return ESP_ERR_INVALID_STATE;
    if (!path || !*path || std::strlen(path) >= sizeof(impl_->rom_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    unload();
    last_error_.store(ESP_OK);
    // The length was validated above, so copy the complete path including NUL.
    std::memcpy(impl_->rom_path, path, std::strlen(path) + 1U);

    esp_err_t error = loadRomFile(path);
    if (error == ESP_OK) error = initializeCore();
    if (error == ESP_OK) error = initializeSaveRam();
    if (error != ESP_OK) {
        last_error_.store(error);
        g_active_gameboy.store(nullptr, std::memory_order_release);
        freeRomAndSave();
        return error;
    }

    impl_->previous_valid = false;
    impl_->last_draw_sequence = UINT32_MAX;
    impl_->lcd_publish_counter = 0;
    impl_->emulated_frames.store(0);
    impl_->displayed_frames.store(0);
    impl_->audio_frames_pushed.store(0);
    impl_->audio_frames_dropped.store(0);
    impl_->cache_hits.store(0);
    impl_->cache_misses.store(0);
    impl_->save_writes.store(0);
    impl_->last_frame_us.store(0);
    impl_->max_frame_us.store(0);
    frame_sequence_.store(0);
    button_mask_.store(0);
    stop_requested_.store(false);
    loaded_.store(true);
    g_active_gameboy.store(this, std::memory_order_release);

    error = audio_->startRealtimeStereo(GB_AUDIO_SOURCE_RATE, config_.realtime_volume);
    if (error != ESP_OK) {
        loaded_.store(false);
        g_active_gameboy.store(nullptr, std::memory_order_release);
        freeRomAndSave();
        last_error_.store(error);
        return error;
    }

    // Start with a tiny silent cushion so the independent Core 0 I2S task does
    // not underrun while the first emulated frame is being produced.
    std::memset(impl_->audio_samples, 0, sizeof(impl_->audio_samples));
    audio_->pushRealtimeStereo(
        reinterpret_cast<const int16_t*>(impl_->audio_samples), AUDIO_SAMPLES);
    audio_->pushRealtimeStereo(
        reinterpret_cast<const int16_t*>(impl_->audio_samples), AUDIO_SAMPLES);

    task_running_.store(true);
    const BaseType_t created = xTaskCreatePinnedToCore(
        taskEntry, "pogopo_gb", config_.task_stack, this,
        config_.task_priority, &task_, config_.task_core);
    if (created != pdPASS) {
        task_running_.store(false);
        audio_->stopRealtime();
        loaded_.store(false);
        g_active_gameboy.store(nullptr, std::memory_order_release);
        freeRomAndSave();
        last_error_.store(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Loaded %s (%lu bytes, %s, cache=%u pages, save=%lu)",
             impl_->rom_title[0] ? impl_->rom_title : impl_->rom_path,
             static_cast<unsigned long>(impl_->rom_size),
             impl_->rom_in_psram ? "PSRAM" : "internal RAM",
             static_cast<unsigned>(impl_->rom_cache_pages),
             static_cast<unsigned long>(impl_->save_size));
    return ESP_OK;
}

void GameBoy::unload() {
    if (!impl_) return;

    stop_requested_.store(true);
    for (int wait = 0; wait < 150 && task_running_.load(); ++wait) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (task_running_.load() && task_) {
        vTaskDelete(task_);
        task_running_.store(false);
    }
    task_ = nullptr;

    if (audio_) audio_->stopRealtime();
    if (loaded_.load()) flushSave();
    loaded_.store(false);
    g_active_gameboy.store(nullptr, std::memory_order_release);
    freeRomAndSave();
    stop_requested_.store(false);
    button_mask_.store(0);
}

esp_err_t GameBoy::loadRomFile(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return ESP_FAIL;
    }
    const long length = std::ftell(file);
    if (length <= 0 || length > 8L * 1024L * 1024L) {
        std::fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    std::rewind(file);
    impl_->rom_size = static_cast<uint32_t>(length);

    if (impl_->rom_size <= config_.internal_rom_limit) {
        impl_->rom_data = static_cast<uint8_t*>(heap_caps_malloc(
            impl_->rom_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!impl_->rom_data) {
        impl_->rom_data = static_cast<uint8_t*>(heap_caps_malloc(
            impl_->rom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        impl_->rom_in_psram = impl_->rom_data != nullptr;
    }
    if (!impl_->rom_data) {
        std::fclose(file);
        return ESP_ERR_NO_MEM;
    }

    const size_t read = std::fread(impl_->rom_data, 1, impl_->rom_size, file);
    std::fclose(file);
    if (read != impl_->rom_size) return ESP_ERR_INVALID_SIZE;

    if (impl_->rom_size >= 0x150U) {
        ESP_LOGI(TAG, "Cart header: type=0x%02X romCode=0x%02X ramCode=0x%02X",
                 impl_->rom_data[0x147], impl_->rom_data[0x148], impl_->rom_data[0x149]);
    }
    initializeRomCache();
    return ESP_OK;
}

bool GameBoy::initializeRomCache() {
    if (!impl_->rom_in_psram) return true;

    const uint8_t candidates[] = {8, 6, 4, 2};
    for (uint8_t candidate : candidates) {
        const uint8_t pages = std::min(candidate, config_.requested_cache_pages);
        if (pages == 0 || pages > MAX_CACHE_PAGES) continue;
        impl_->rom_cache = static_cast<uint8_t*>(heap_caps_malloc(
            static_cast<size_t>(pages) * ROM_CACHE_PAGE_SIZE,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (impl_->rom_cache) {
            impl_->rom_cache_pages = pages;
            return true;
        }
    }
    impl_->rom_cache_pages = 0;
    return false;
}

uint8_t GameBoy::readRom(uint32_t address) {
    if (!impl_ || !impl_->rom_data || address >= impl_->rom_size) return 0xFF;
    if (!impl_->rom_in_psram || !impl_->rom_cache || impl_->rom_cache_pages == 0) {
        return impl_->rom_data[address];
    }

    const uint32_t page = address / ROM_CACHE_PAGE_SIZE;
    const uint32_t offset = address & (ROM_CACHE_PAGE_SIZE - 1U);
    ++impl_->cache_tick;

    if (page == impl_->cache_last_page && impl_->cache_last_pointer) {
        impl_->cache_hits.fetch_add(1);
        return impl_->cache_last_pointer[offset];
    }

    for (uint8_t i = 0; i < impl_->rom_cache_pages; ++i) {
        if (impl_->cache_page_index[i] == page) {
            impl_->cache_age[i] = impl_->cache_tick;
            impl_->cache_last_page = page;
            impl_->cache_last_pointer = impl_->rom_cache +
                static_cast<size_t>(i) * ROM_CACHE_PAGE_SIZE;
            impl_->cache_hits.fetch_add(1);
            return impl_->cache_last_pointer[offset];
        }
    }

    impl_->cache_misses.fetch_add(1);
    uint8_t slot = 0;
    for (uint8_t i = 1; i < impl_->rom_cache_pages; ++i) {
        if (impl_->cache_page_index[i] == UINT32_MAX ||
            impl_->cache_age[i] < impl_->cache_age[slot]) {
            slot = i;
        }
    }

    const uint32_t page_start = page * ROM_CACHE_PAGE_SIZE;
    const uint32_t copy_bytes = std::min<uint32_t>(ROM_CACHE_PAGE_SIZE,
                                                    impl_->rom_size - page_start);
    uint8_t* destination = impl_->rom_cache +
        static_cast<size_t>(slot) * ROM_CACHE_PAGE_SIZE;
    std::memcpy(destination, impl_->rom_data + page_start, copy_bytes);
    if (copy_bytes < ROM_CACHE_PAGE_SIZE) {
        std::memset(destination + copy_bytes, 0xFF, ROM_CACHE_PAGE_SIZE - copy_bytes);
    }
    impl_->cache_page_index[slot] = page;
    impl_->cache_age[slot] = impl_->cache_tick;
    impl_->cache_last_page = page;
    impl_->cache_last_pointer = destination;
    return destination[offset];
}

esp_err_t GameBoy::initializeCore() {
    std::memset(&impl_->core, 0, sizeof(impl_->core));
    std::memset(&impl_->apu, 0, sizeof(impl_->apu));
    g_active_gameboy.store(this, std::memory_order_release);

    const auto rom_read = [](gb_s* core, const uint_fast32_t address) -> uint8_t {
        auto* self = static_cast<GameBoy*>(core->direct.priv);
        return self ? self->readRom(static_cast<uint32_t>(address)) : 0xFF;
    };
    const auto ram_read = [](gb_s* core, const uint_fast32_t address) -> uint8_t {
        auto* self = static_cast<GameBoy*>(core->direct.priv);
        if (!self || !self->impl_ || !self->impl_->save_data ||
            address >= self->impl_->save_size) return 0xFF;
        return self->impl_->save_data[address];
    };
    const auto ram_write = [](gb_s* core, const uint_fast32_t address, const uint8_t value) {
        auto* self = static_cast<GameBoy*>(core->direct.priv);
        if (!self || !self->impl_ || !self->impl_->save_data ||
            address >= self->impl_->save_size) return;
        self->impl_->save_data[address] = value;
        self->impl_->save_dirty = true;
    };
    const auto error = [](gb_s* core, const gb_error_e gb_error, const uint16_t value) {
        auto* self = static_cast<GameBoy*>(core->direct.priv);
        if (self) self->last_error_.store(ESP_FAIL);
        ESP_LOGW(TAG, "Peanut-GB error=%d value=0x%04X", static_cast<int>(gb_error), value);
    };

    const gb_init_error_e init = gb_init(
        &impl_->core, rom_read, ram_read, ram_write, error, this);
    if (init != GB_INIT_NO_ERROR) {
        ESP_LOGE(TAG, "gb_init failed: %d", static_cast<int>(init));
        return ESP_ERR_INVALID_RESPONSE;
    }

    const auto lcd_line = [](gb_s* core, const uint8_t* pixels, const uint_fast8_t line) {
        auto* self = static_cast<GameBoy*>(core->direct.priv);
        if (!self || !self->impl_ || line >= SCREEN_HEIGHT) return;
        uint8_t* destination = self->impl_->frame_back +
            static_cast<size_t>(line) * SCREEN_WIDTH;
        for (int x = 0; x < SCREEN_WIDTH; ++x) {
            // Peanut-GB uses 0=black ... 3=white. Pogopo's generic indexed
            // renderer uses 0=white ... 3=black, so normalize once here.
            destination[x] = static_cast<uint8_t>(3U - (pixels[x] & 0x03U));
        }
        if (line == SCREEN_HEIGHT - 1) {
            ++self->impl_->lcd_publish_counter;
            if (self->impl_->lcd_publish_counter >= self->config_.display_divider) {
                self->impl_->lcd_publish_counter = 0;
                xSemaphoreTake(self->impl_->frame_mutex, portMAX_DELAY);
                std::swap(self->impl_->frame_front, self->impl_->frame_back);
                self->frame_sequence_.fetch_add(1);
                xSemaphoreGive(self->impl_->frame_mutex);
            }
        }
    };
    gb_init_lcd(&impl_->core, lcd_line);
    impl_->core.direct.frame_skip = config_.peanut_frame_skip;
    impl_->core.direct.interlace = false;
    minigb_apu_audio_init(&impl_->apu);

    char title[sizeof(impl_->rom_title)]{};
    const char* result = gb_get_rom_name(&impl_->core, title);
    if (result && *result) {
        const size_t title_length = std::min(std::strlen(result), sizeof(impl_->rom_title) - 1U);
        std::memcpy(impl_->rom_title, result, title_length);
        impl_->rom_title[title_length] = '\0';
    } else {
        constexpr char fallback_title[] = "GAME BOY";
        std::memcpy(impl_->rom_title, fallback_title, sizeof(fallback_title));
    }
    return ESP_OK;
}

esp_err_t GameBoy::initializeSaveRam() {
    size_t requested_size = 0;
    if (gb_get_save_size_s(&impl_->core, &requested_size) != 0) {
        ESP_LOGW(TAG, "Invalid or unsupported cartridge save size");
        requested_size = 0;
    }
    const uint32_t requested = static_cast<uint32_t>(
        std::min<size_t>(requested_size, UINT32_MAX));
    impl_->save_size = std::min(requested, MAX_SAVE_BYTES);
    if (impl_->save_size == 0) return ESP_OK;

    impl_->save_data = allocate_bytes(impl_->save_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!impl_->save_data) return ESP_ERR_NO_MEM;
    std::memset(impl_->save_data, 0xFF, impl_->save_size);

    const size_t path_length = std::strlen(impl_->rom_path);
    if (path_length + 5U >= sizeof(impl_->save_path)) return ESP_ERR_INVALID_SIZE;
    std::memcpy(impl_->save_path, impl_->rom_path, path_length + 1U);
    char* dot = std::strrchr(impl_->save_path, '.');
    if (dot) std::strcpy(dot, ".sav");
    else std::strcat(impl_->save_path, ".sav");

    FILE* save = std::fopen(impl_->save_path, "rb");
    if (save) {
        const size_t read = std::fread(impl_->save_data, 1, impl_->save_size, save);
        std::fclose(save);
        ESP_LOGI(TAG, "Loaded save %s (%u/%lu bytes)", impl_->save_path,
                 static_cast<unsigned>(read), static_cast<unsigned long>(impl_->save_size));
        (void)read;
    }
    impl_->save_dirty = false;
    impl_->last_save_us = esp_timer_get_time();
    return ESP_OK;
}

void GameBoy::freeRomAndSave() {
    if (!impl_) return;
    if (impl_->rom_data) heap_caps_free(impl_->rom_data);
    if (impl_->rom_cache) heap_caps_free(impl_->rom_cache);
    if (impl_->save_data) heap_caps_free(impl_->save_data);
    impl_->rom_data = nullptr;
    impl_->rom_cache = nullptr;
    impl_->save_data = nullptr;
    impl_->rom_size = 0;
    impl_->save_size = 0;
    impl_->rom_in_psram = false;
    impl_->rom_cache_pages = 0;
    impl_->save_dirty = false;
    impl_->rom_path[0] = '\0';
    impl_->save_path[0] = '\0';
    impl_->rom_title[0] = '\0';
    impl_->cache_tick = 0;
    impl_->cache_last_page = UINT32_MAX;
    impl_->cache_last_pointer = nullptr;
    for (uint8_t i = 0; i < MAX_CACHE_PAGES; ++i) {
        impl_->cache_page_index[i] = UINT32_MAX;
        impl_->cache_age[i] = 0;
    }
}

esp_err_t GameBoy::flushSave() {
    if (!impl_ || !impl_->save_data || impl_->save_size == 0 ||
        !impl_->save_dirty || impl_->save_path[0] == '\0') {
        return ESP_OK;
    }

    xSemaphoreTake(impl_->core_mutex, portMAX_DELAY);
    FILE* save = std::fopen(impl_->save_path, "wb");
    if (!save) {
        xSemaphoreGive(impl_->core_mutex);
        return ESP_FAIL;
    }
    const size_t written = std::fwrite(impl_->save_data, 1, impl_->save_size, save);
    std::fclose(save);
    if (written == impl_->save_size) {
        impl_->save_dirty = false;
        impl_->last_save_us = esp_timer_get_time();
        impl_->save_writes.fetch_add(1);
        ESP_LOGI(TAG, "Save flushed: %s (%lu bytes)", impl_->save_path,
                 static_cast<unsigned long>(impl_->save_size));
        xSemaphoreGive(impl_->core_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(impl_->core_mutex);
    return ESP_ERR_INVALID_SIZE;
}

void GameBoy::reset() {
    if (!impl_ || !loaded_.load()) return;
    xSemaphoreTake(impl_->core_mutex, portMAX_DELAY);
    gb_reset(&impl_->core);
    minigb_apu_audio_init(&impl_->apu);
    xSemaphoreGive(impl_->core_mutex);
    if (audio_) audio_->startRealtimeStereo(GB_AUDIO_SOURCE_RATE, config_.realtime_volume);
    impl_->previous_valid = false;
}

void GameBoy::setButtons(const Buttons& buttons) {
    uint8_t mask = 0;
    if (buttons.up) mask |= 1U << 0U;
    if (buttons.down) mask |= 1U << 1U;
    if (buttons.left) mask |= 1U << 2U;
    if (buttons.right) mask |= 1U << 3U;
    if (buttons.a) mask |= 1U << 4U;
    if (buttons.b) mask |= 1U << 5U;
    if (buttons.start) mask |= 1U << 6U;
    if (buttons.select) mask |= 1U << 7U;
    button_mask_.store(mask, std::memory_order_release);
}

void GameBoy::setCoreButtons(uint8_t mask) {
    impl_->core.direct.joypad_bits.up = (mask & (1U << 0U)) ? 0 : 1;
    impl_->core.direct.joypad_bits.down = (mask & (1U << 1U)) ? 0 : 1;
    impl_->core.direct.joypad_bits.left = (mask & (1U << 2U)) ? 0 : 1;
    impl_->core.direct.joypad_bits.right = (mask & (1U << 3U)) ? 0 : 1;
    impl_->core.direct.joypad_bits.a = (mask & (1U << 4U)) ? 0 : 1;
    impl_->core.direct.joypad_bits.b = (mask & (1U << 5U)) ? 0 : 1;
    impl_->core.direct.joypad_bits.start = (mask & (1U << 6U)) ? 0 : 1;
    impl_->core.direct.joypad_bits.select = (mask & (1U << 7U)) ? 0 : 1;
}

void GameBoy::taskEntry(void* argument) {
    static_cast<GameBoy*>(argument)->taskLoop();
}

void GameBoy::taskLoop() {
    int64_t next_frame_us = esp_timer_get_time();

    while (!stop_requested_.load()) {
        next_frame_us += FRAME_PERIOD_US;
        const int64_t frame_start = esp_timer_get_time();

        xSemaphoreTake(impl_->core_mutex, portMAX_DELAY);
        setCoreButtons(button_mask_.load(std::memory_order_acquire));
        gb_run_frame(&impl_->core);
        pushAudioFrame();
        xSemaphoreGive(impl_->core_mutex);

        impl_->emulated_frames.fetch_add(1);
        const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - frame_start);
        impl_->last_frame_us.store(elapsed);
        uint32_t previous_max = impl_->max_frame_us.load();
        while (elapsed > previous_max &&
               !impl_->max_frame_us.compare_exchange_weak(previous_max, elapsed)) {
        }

        if (impl_->save_dirty && config_.save_flush_interval_ms > 0 &&
            esp_timer_get_time() - impl_->last_save_us >=
                static_cast<int64_t>(config_.save_flush_interval_ms) * 1000LL) {
            // We already own core_mutex. Write directly to avoid recursive lock.
            FILE* save = std::fopen(impl_->save_path, "wb");
            if (save) {
                const size_t written = std::fwrite(impl_->save_data, 1, impl_->save_size, save);
                std::fclose(save);
                if (written == impl_->save_size) {
                    impl_->save_dirty = false;
                    impl_->last_save_us = esp_timer_get_time();
                    impl_->save_writes.fetch_add(1);
                }
            }
        }

        int64_t remaining = next_frame_us - esp_timer_get_time();
        if (remaining > 2000) {
            vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(remaining / 1000)));
        }
        while (!stop_requested_.load() && esp_timer_get_time() < next_frame_us) {
            taskYIELD();
        }
        if (esp_timer_get_time() - next_frame_us > FRAME_PERIOD_US * 3) {
            next_frame_us = esp_timer_get_time();
        }
    }

    task_running_.store(false);
    vTaskDelete(nullptr);
}

void GameBoy::pushAudioFrame() {
    if (!audio_ || !audio_->enabled()) return;
    minigb_apu_audio_callback(&impl_->apu, impl_->audio_samples);
    const size_t pushed = audio_->pushRealtimeStereo(
        reinterpret_cast<const int16_t*>(impl_->audio_samples), AUDIO_SAMPLES);
    impl_->audio_frames_pushed.fetch_add(static_cast<uint32_t>(pushed));
    if (pushed < AUDIO_SAMPLES) {
        impl_->audio_frames_dropped.fetch_add(static_cast<uint32_t>(AUDIO_SAMPLES - pushed));
    }
}

bool GameBoy::drawLatest(gfx::Canvas& canvas, ScaleMode mode, bool force_full) {
    if (!impl_ || !loaded_.load()) return false;
    const uint32_t sequence = frame_sequence_.load(std::memory_order_acquire);
    if (!force_full && sequence == impl_->last_draw_sequence && impl_->previous_valid &&
        mode == impl_->previous_scale) {
        return false;
    }

    xSemaphoreTake(impl_->frame_mutex, portMAX_DELAY);
    std::memcpy(impl_->frame_render, impl_->frame_front, FRAME_PIXELS);
    xSemaphoreGive(impl_->frame_mutex);

    const bool full = force_full || !impl_->previous_valid || mode != impl_->previous_scale;
    if (full) canvas.clear(gfx::WHITE);

    int x = 0;
    int y = 0;
    int width = SCREEN_WIDTH;
    int height = SCREEN_HEIGHT;
    if (mode == ScaleMode::FitHeight) {
        width = 267;
        height = 240;
        x = (canvas.width() - width) / 2;
    } else {
        x = (canvas.width() - width) / 2;
        y = (canvas.height() - height) / 2;
    }

    canvas.draw_indexed2_fast(
        x, y, SCREEN_WIDTH, SCREEN_HEIGHT, impl_->frame_render, width, height,
        config_.dither, false);
    impl_->previous_valid = true;
    impl_->previous_scale = mode;
    impl_->last_draw_sequence = sequence;
    impl_->displayed_frames.fetch_add(1);
    return true;
}

uint8_t GameBoy::apuRead(uint16_t address) {
    return impl_ ? minigb_apu_audio_read(&impl_->apu, address) : 0xFF;
}

void GameBoy::apuWrite(uint16_t address, uint8_t value) {
    if (impl_) minigb_apu_audio_write(&impl_->apu, address, value);
}

const char* GameBoy::romTitle() const {
    return impl_ ? impl_->rom_title : "";
}

const char* GameBoy::romPath() const {
    return impl_ ? impl_->rom_path : "";
}

Stats GameBoy::stats() const {
    Stats result;
    if (!impl_) return result;
    result.emulated_frames = impl_->emulated_frames.load();
    result.displayed_frames = impl_->displayed_frames.load();
    result.audio_frames_pushed = impl_->audio_frames_pushed.load();
    result.audio_frames_dropped = impl_->audio_frames_dropped.load();
    result.cache_hits = impl_->cache_hits.load();
    result.cache_misses = impl_->cache_misses.load();
    result.save_writes = impl_->save_writes.load();
    result.last_frame_us = impl_->last_frame_us.load();
    result.max_frame_us = impl_->max_frame_us.load();
    result.rom_bytes = impl_->rom_size;
    result.save_bytes = impl_->save_size;
    result.cache_pages = impl_->rom_cache_pages;
    result.rom_in_psram = impl_->rom_in_psram;
    return result;
}

const char* scale_mode_name(ScaleMode mode) {
    return mode == ScaleMode::OneX ? "1X FAST" : "FIT 240";
}

} // namespace pogopo::gameboy
