#include "pogopo/gameboy_advance/gameboy_advance.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

extern "C" {
#include "common.h"
#include "gba_memory.h"
#include "input.h"
#include "libretro.h"
#include "main.h"
#include "serial.h"
#include "sound.h"
#include "video.h"
}

namespace {

constexpr char TAG[] = "pogopo_gba";
constexpr int64_t FRAME_PERIOD_US = 16667;
constexpr uint32_t MAX_ROM_BYTES = 32U * 1024U * 1024U;
constexpr uint32_t MAX_SAVE_BYTES = 128U * 1024U;
constexpr uint32_t AUDIO_SOURCE_RATE = 32768;
constexpr uint32_t AUDIO_PACKET_FRAMES = 512;
constexpr uint32_t AUDIO_TEMP_FRAMES = 640;
constexpr uint32_t AUDIO_TARGET_FRAMES = AUDIO_PACKET_FRAMES * 4U;

constexpr uint16_t BTN_UP = 1U << 0U;
constexpr uint16_t BTN_DOWN = 1U << 1U;
constexpr uint16_t BTN_LEFT = 1U << 2U;
constexpr uint16_t BTN_RIGHT = 1U << 3U;
constexpr uint16_t BTN_A = 1U << 4U;
constexpr uint16_t BTN_B = 1U << 5U;
constexpr uint16_t BTN_START = 1U << 6U;
constexpr uint16_t BTN_SELECT = 1U << 7U;
constexpr uint16_t BTN_L = 1U << 8U;
constexpr uint16_t BTN_R = 1U << 9U;

pogopo::gameboy_advance::GameBoyAdvance* g_active_gba = nullptr;

int16_t gba_input_callback(unsigned, unsigned, unsigned, unsigned id) {
    const uint16_t buttons = g_active_gba ? g_active_gba->coreButtonMask() : 0;
    uint16_t result = 0;
    if (buttons & BTN_DOWN) result |= 1U << RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (buttons & BTN_UP) result |= 1U << RETRO_DEVICE_ID_JOYPAD_UP;
    if (buttons & BTN_LEFT) result |= 1U << RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (buttons & BTN_RIGHT) result |= 1U << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (buttons & BTN_START) result |= 1U << RETRO_DEVICE_ID_JOYPAD_START;
    if (buttons & BTN_SELECT) result |= 1U << RETRO_DEVICE_ID_JOYPAD_SELECT;
    if (buttons & BTN_B) result |= 1U << RETRO_DEVICE_ID_JOYPAD_B;
    if (buttons & BTN_A) result |= 1U << RETRO_DEVICE_ID_JOYPAD_A;
    if (buttons & BTN_L) result |= 1U << RETRO_DEVICE_ID_JOYPAD_L;
    if (buttons & BTN_R) result |= 1U << RETRO_DEVICE_ID_JOYPAD_R;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return static_cast<int16_t>(result);
    return (result & (1U << id)) ? 1 : 0;
}

void copy_path(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0) return;
    destination[0] = '\0';
    if (!source) return;
    const size_t count = std::min(capacity - 1U, std::strlen(source));
    std::memcpy(destination, source, count);
    destination[count] = '\0';
}

} // namespace

extern "C" {

gbsp_memory_t* gbsp_memory = nullptr;
u32 idle_loop_target_pc = 0xFFFFFFFFU;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES]{};
u32 translation_gate_targets = 0;
u32 skip_next_frame = 0;
int dynarec_enable = 0;
boot_mode selected_boot_mode = boot_game;
int sprite_limit = 1;

void netpacket_poll_receive() {}
void netpacket_send(uint16_t, const void*, size_t) {}
void set_fastforward_override(bool) {}

} // extern "C"

namespace pogopo::gameboy_advance {

struct GameBoyAdvance::Impl {
    uint16_t* frame_front = nullptr;
    uint16_t* frame_back = nullptr;
    uint16_t* frame_render = nullptr;
    SemaphoreHandle_t frame_mutex = nullptr;
    SemaphoreHandle_t core_mutex = nullptr;

    int16_t audio_samples[AUDIO_TEMP_FRAMES * 2U]{};
    char rom_path[192]{};
    char save_path[192]{};
    char rom_title[32]{};
    uint32_t rom_size = 0;
    uint8_t** internal_memory_map = nullptr;
    bool core_memory_ready = false;
    bool previous_valid = false;
    ScaleMode previous_scale = ScaleMode::OneX;
    uint32_t last_draw_sequence = UINT32_MAX;
    uint32_t behind_yield_counter = 0;

    std::atomic<uint32_t> emulated_frames{0};
    std::atomic<uint32_t> rendered_frames{0};
    std::atomic<uint32_t> displayed_frames{0};
    std::atomic<uint32_t> audio_frames_pushed{0};
    std::atomic<uint32_t> audio_frames_dropped{0};
    std::atomic<uint32_t> page_loads{0};
    std::atomic<uint32_t> page_load_us{0};
    std::atomic<uint32_t> save_writes{0};
    std::atomic<uint32_t> last_frame_us{0};
    std::atomic<uint32_t> max_frame_us{0};
};

GameBoyAdvance::~GameBoyAdvance() {
    end();
}

esp_err_t GameBoyAdvance::begin(audio::Audio& audio) {
    return begin(audio, Config{});
}

esp_err_t GameBoyAdvance::begin(audio::Audio& audio, const Config& config) {
    if (initialized_.load() || impl_) return ESP_ERR_INVALID_STATE;
    if (!audio.ok() || config.rom_buffer_megabytes == 0 ||
        config.rom_buffer_megabytes > 6 || config.task_stack < 8192) {
        return ESP_ERR_INVALID_ARG;
    }

    void* storage = heap_caps_malloc(sizeof(Impl), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!storage) return ESP_ERR_NO_MEM;
    impl_ = new (storage) Impl{};
    impl_->frame_mutex = xSemaphoreCreateMutex();
    impl_->core_mutex = xSemaphoreCreateMutex();
    if (!impl_->frame_mutex || !impl_->core_mutex) {
        end();
        return ESP_ERR_NO_MEM;
    }

    config_ = config;
    audio_ = &audio;
    initialized_.store(true);
    last_error_.store(ESP_OK);
    ESP_LOGI(TAG, "Experimental GBA frontend ready: gpSP interpreter, cache up to %u MiB",
             static_cast<unsigned>(config_.rom_buffer_megabytes));
    return ESP_OK;
}

void GameBoyAdvance::end() {
    unload();
    initialized_.store(false);
    audio_ = nullptr;
    if (!impl_) return;
    if (impl_->frame_mutex) vSemaphoreDelete(impl_->frame_mutex);
    if (impl_->core_mutex) vSemaphoreDelete(impl_->core_mutex);
    impl_->~Impl();
    heap_caps_free(impl_);
    impl_ = nullptr;
}

esp_err_t GameBoyAdvance::allocateCoreMemory() {
    if (!impl_ || gbsp_memory) return ESP_ERR_INVALID_STATE;

    gbsp_memory = static_cast<gbsp_memory_t*>(heap_caps_calloc(
        1, sizeof(gbsp_memory_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!gbsp_memory) return ESP_ERR_NO_MEM;
    impl_->internal_memory_map = static_cast<uint8_t**>(heap_caps_calloc(
        8U * 1024U, sizeof(uint8_t*), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    memory_map_read = impl_->internal_memory_map
        ? impl_->internal_memory_map : gbsp_memory->memory_map_read;

    impl_->frame_front = static_cast<uint16_t*>(heap_caps_malloc(
        FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    impl_->frame_back = static_cast<uint16_t*>(heap_caps_malloc(
        FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    impl_->frame_render = static_cast<uint16_t*>(heap_caps_malloc(
        FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!impl_->frame_front || !impl_->frame_back || !impl_->frame_render) {
        freeCoreMemory();
        return ESP_ERR_NO_MEM;
    }
    std::fill_n(impl_->frame_front, FRAME_PIXELS, static_cast<uint16_t>(0xFFFF));
    std::fill_n(impl_->frame_back, FRAME_PIXELS, static_cast<uint16_t>(0xFFFF));
    std::fill_n(impl_->frame_render, FRAME_PIXELS, static_cast<uint16_t>(0xFFFF));
    gba_screen_pixels = impl_->frame_back;
    impl_->core_memory_ready = true;
    return ESP_OK;
}

void GameBoyAdvance::freeCoreMemory() {
    if (!impl_) return;
    if (gamepak_buffer_count > 0 || gamepak_file_large) memory_term();
    gamepak_buffer_count = 0;
    gba_screen_pixels = nullptr;
    memory_map_read = nullptr;
    if (impl_->frame_front) heap_caps_free(impl_->frame_front);
    if (impl_->frame_back) heap_caps_free(impl_->frame_back);
    if (impl_->frame_render) heap_caps_free(impl_->frame_render);
    if (impl_->internal_memory_map) heap_caps_free(impl_->internal_memory_map);
    impl_->frame_front = nullptr;
    impl_->frame_back = nullptr;
    impl_->frame_render = nullptr;
    impl_->internal_memory_map = nullptr;
    if (gbsp_memory) heap_caps_free(gbsp_memory);
    gbsp_memory = nullptr;
    impl_->core_memory_ready = false;
}

esp_err_t GameBoyAdvance::load(const char* path) {
    if (!initialized_.load() || !impl_ || !audio_) return ESP_ERR_INVALID_STATE;
    if (!path || !*path || std::strlen(path) >= sizeof(impl_->rom_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    unload();
    copy_path(impl_->rom_path, sizeof(impl_->rom_path), path);
    FILE* rom_file = std::fopen(path, "rb");
    if (!rom_file) return ESP_ERR_NOT_FOUND;
    if (std::fseek(rom_file, 0, SEEK_END) != 0) {
        std::fclose(rom_file);
        return ESP_FAIL;
    }
    const long rom_length = std::ftell(rom_file);
    std::fclose(rom_file);
    if (rom_length <= 0 || static_cast<uint64_t>(rom_length) > MAX_ROM_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    impl_->rom_size = static_cast<uint32_t>(rom_length);

    esp_err_t error = allocateCoreMemory();
    if (error != ESP_OK) {
        last_error_.store(error);
        return error;
    }

    const uint32_t blocks_needed = std::max<uint32_t>(
        1U, (impl_->rom_size + 1024U * 1024U - 1U) / (1024U * 1024U));
    gamepak_buffer_limit = std::min<uint32_t>(config_.rom_buffer_megabytes, blocks_needed);
    init_gamepak_buffer();
    if (gamepak_buffer_count == 0) error = ESP_ERR_NO_MEM;
    if (error == ESP_OK && load_gamepak(nullptr, path, FEAT_AUTODETECT,
                                        FEAT_AUTODETECT, SERIAL_MODE_DISABLED) != 0) {
        error = ESP_ERR_INVALID_RESPONSE;
    }

    if (error == ESP_OK) {
        std::memset(gamepak_backup, 0xFF, MAX_SAVE_BYTES);
        error = loadSave();
    }
    if (error == ESP_OK) {
        libretro_supports_bitmasks = true;
        retro_set_input_state(gba_input_callback);
        sound_master_enable = true;
        init_sound();
        reset_gba();
        clear_gamepak_stickybits();

        const uint8_t* header = gamepak_buffers[0];
        size_t title_length = 12;
        while (title_length > 0 &&
               (header[0xA0 + title_length - 1U] == ' ' ||
                header[0xA0 + title_length - 1U] == '\0')) {
            --title_length;
        }
        title_length = std::min(title_length, sizeof(impl_->rom_title) - 1U);
        for (size_t i = 0; i < title_length; ++i) {
            const uint8_t character = header[0xA0 + i];
            impl_->rom_title[i] = character >= 0x20 && character <= 0x7E
                ? static_cast<char>(character) : '_';
        }
        impl_->rom_title[title_length] = '\0';
        if (title_length == 0) copy_path(impl_->rom_title, sizeof(impl_->rom_title), "GAME BOY ADVANCE");
    }

    if (error != ESP_OK) {
        freeCoreMemory();
        impl_->rom_path[0] = '\0';
        impl_->save_path[0] = '\0';
        impl_->rom_title[0] = '\0';
        impl_->rom_size = 0;
        last_error_.store(error);
        return error;
    }

    impl_->previous_valid = false;
    impl_->last_draw_sequence = UINT32_MAX;
    impl_->behind_yield_counter = 0;
    impl_->emulated_frames.store(0);
    impl_->rendered_frames.store(0);
    impl_->displayed_frames.store(0);
    impl_->audio_frames_pushed.store(0);
    impl_->audio_frames_dropped.store(0);
    impl_->page_loads.store(0);
    impl_->page_load_us.store(0);
    impl_->save_writes.store(0);
    impl_->last_frame_us.store(0);
    impl_->max_frame_us.store(0);
    frame_sequence_.store(0);
    button_mask_.store(0);
    paused_.store(false);
    stop_requested_.store(false);
    loaded_.store(true);
    g_active_gba = this;

    error = startAudio();
    if (error != ESP_OK) {
        loaded_.store(false);
        g_active_gba = nullptr;
        freeCoreMemory();
        last_error_.store(error);
        return error;
    }

    task_running_.store(true);
    const BaseType_t created = xTaskCreatePinnedToCore(
        taskEntry, "pogopo_gba", config_.task_stack, this,
        config_.task_priority, &task_, config_.task_core);
    if (created != pdPASS) {
        task_running_.store(false);
        audio_->stopRealtime();
        loaded_.store(false);
        g_active_gba = nullptr;
        freeCoreMemory();
        last_error_.store(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "Loaded %s (%lu bytes, cache=%lu MiB, swap=%s, map=%s, state=%u bytes, frames=%u bytes)",
             impl_->rom_title, static_cast<unsigned long>(impl_->rom_size),
             static_cast<unsigned long>(gamepak_buffer_count),
             gamepak_must_swap() ? "SD" : "FULL",
             impl_->internal_memory_map ? "INT" : "PSRAM",
             static_cast<unsigned>(sizeof(gbsp_memory_t)),
             static_cast<unsigned>(FRAME_BYTES * 3U));
    last_error_.store(ESP_OK);
    return ESP_OK;
}

void GameBoyAdvance::stopTask() {
    stop_requested_.store(true);
    for (int wait = 0; wait < 200 && task_running_.load(); ++wait) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (task_running_.load() && task_) {
        vTaskDelete(task_);
        task_running_.store(false);
    }
    task_ = nullptr;
}

void GameBoyAdvance::unload() {
    if (!impl_) return;
    stopTask();
    if (audio_) audio_->stopRealtime();
    if (loaded_.load()) flushSave();
    loaded_.store(false);
    g_active_gba = nullptr;
    freeCoreMemory();
    stop_requested_.store(false);
    paused_.store(false);
    button_mask_.store(0);
    impl_->rom_path[0] = '\0';
    impl_->save_path[0] = '\0';
    impl_->rom_title[0] = '\0';
    impl_->rom_size = 0;
}

esp_err_t GameBoyAdvance::loadSave() {
    const size_t length = std::strlen(impl_->rom_path);
    if (length + 5U >= sizeof(impl_->save_path)) return ESP_ERR_INVALID_SIZE;
    copy_path(impl_->save_path, sizeof(impl_->save_path), impl_->rom_path);
    char* dot = std::strrchr(impl_->save_path, '.');
    if (dot) std::strcpy(dot, ".sav");
    else std::strcat(impl_->save_path, ".sav");

    FILE* save = std::fopen(impl_->save_path, "rb");
    if (!save) return ESP_OK;
    const size_t read = std::fread(gamepak_backup, 1, MAX_SAVE_BYTES, save);
    std::fclose(save);
    ESP_LOGI(TAG, "Loaded battery save %s (%u bytes)", impl_->save_path,
             static_cast<unsigned>(read));
    return ESP_OK;
}

uint32_t GameBoyAdvance::activeSaveSize() const {
    if (!loaded_.load() || !gbsp_memory) return 0;
    if (backup_type == BACKUP_EEPROM) {
        return eeprom_size == EEPROM_8_KBYTE ? 8U * 1024U : 512U;
    }
    if (backup_type == BACKUP_FLASH) {
        return flash_bank_cnt == FLASH_SIZE_128KB ? 128U * 1024U : 64U * 1024U;
    }
    if (backup_type == BACKUP_SRAM) return 32U * 1024U;
    return MAX_SAVE_BYTES;
}

esp_err_t GameBoyAdvance::flushSave() {
    if (!impl_ || !loaded_.load() || !gbsp_memory || impl_->save_path[0] == '\0') {
        return ESP_OK;
    }
    xSemaphoreTake(impl_->core_mutex, portMAX_DELAY);
    const uint32_t bytes = activeSaveSize();
    FILE* save = std::fopen(impl_->save_path, "wb");
    if (!save) {
        xSemaphoreGive(impl_->core_mutex);
        return ESP_FAIL;
    }
    const size_t written = std::fwrite(gamepak_backup, 1, bytes, save);
    std::fclose(save);
    xSemaphoreGive(impl_->core_mutex);
    if (written != bytes) return ESP_ERR_INVALID_SIZE;
    impl_->save_writes.fetch_add(1);
    ESP_LOGI(TAG, "Battery save flushed: %s (%lu bytes)", impl_->save_path,
             static_cast<unsigned long>(bytes));
    return ESP_OK;
}

esp_err_t GameBoyAdvance::startAudio() {
    if (!audio_ || !audio_->enabled()) return ESP_OK;
    const esp_err_t error = audio_->startRealtimeStereo(AUDIO_SOURCE_RATE,
                                                        config_.realtime_volume);
    if (error != ESP_OK) return error;
    std::memset(impl_->audio_samples, 0, sizeof(impl_->audio_samples));
    for (int packet = 0; packet < 3; ++packet) {
        audio_->pushRealtimeStereo(impl_->audio_samples, AUDIO_PACKET_FRAMES);
    }
    return ESP_OK;
}

void GameBoyAdvance::reset() {
    if (!impl_ || !loaded_.load()) return;
    xSemaphoreTake(impl_->core_mutex, portMAX_DELAY);
    reset_gba();
    clear_gamepak_stickybits();
    xSemaphoreGive(impl_->core_mutex);
    impl_->previous_valid = false;
}

void GameBoyAdvance::setPaused(bool paused) {
    if (!impl_ || !loaded_.load()) {
        paused_.store(false);
        return;
    }
    if (paused) {
        if (paused_.exchange(true)) return;
        button_mask_.store(0);
        if (audio_) audio_->stopRealtime();
        return;
    }
    if (!paused_.load()) return;
    const esp_err_t error = startAudio();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not resume GBA audio: %s", esp_err_to_name(error));
    }
    paused_.store(false);
}

void GameBoyAdvance::setButtons(const Buttons& buttons) {
    uint16_t mask = 0;
    if (buttons.up) mask |= BTN_UP;
    if (buttons.down) mask |= BTN_DOWN;
    if (buttons.left) mask |= BTN_LEFT;
    if (buttons.right) mask |= BTN_RIGHT;
    if (buttons.a) mask |= BTN_A;
    if (buttons.b) mask |= BTN_B;
    if (buttons.start) mask |= BTN_START;
    if (buttons.select) mask |= BTN_SELECT;
    if (buttons.l) mask |= BTN_L;
    if (buttons.r) mask |= BTN_R;
    button_mask_.store(mask, std::memory_order_release);
}

void GameBoyAdvance::taskEntry(void* argument) {
    static_cast<GameBoyAdvance*>(argument)->taskLoop();
}

void GameBoyAdvance::taskLoop() {
    int64_t next_frame_us = esp_timer_get_time();
    uint32_t local_frame = 0;

    while (!stop_requested_.load()) {
        if (paused_.load()) {
            next_frame_us = esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        next_frame_us += FRAME_PERIOD_US;
        const bool render_frame = !config_.render_every_other_frame ||
                                  ((local_frame & 1U) == 0U);
        const int64_t frame_start = esp_timer_get_time();

        xSemaphoreTake(impl_->core_mutex, portMAX_DELAY);
        skip_next_frame = render_frame ? 0U : 1U;
        update_input();
        rumble_frame_reset();
        clear_gamepak_stickybits();
        execute_arm(execute_cycles);
        xSemaphoreGive(impl_->core_mutex);

        if (render_frame) {
            xSemaphoreTake(impl_->frame_mutex, portMAX_DELAY);
            std::swap(impl_->frame_front, impl_->frame_back);
            gba_screen_pixels = impl_->frame_back;
            frame_sequence_.fetch_add(1, std::memory_order_release);
            xSemaphoreGive(impl_->frame_mutex);
            impl_->rendered_frames.fetch_add(1);
        }

        const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - frame_start);
        impl_->last_frame_us.store(elapsed);
        uint32_t previous_max = impl_->max_frame_us.load();
        while (elapsed > previous_max &&
               !impl_->max_frame_us.compare_exchange_weak(previous_max, elapsed)) {
        }
        impl_->emulated_frames.fetch_add(1);
        impl_->page_loads.store(gamepak_page_loads, std::memory_order_relaxed);
        impl_->page_load_us.store(gamepak_page_load_us, std::memory_order_relaxed);

        if (audio_ && audio_->enabled()) {
            const uint32_t frames = sound_read_samples(impl_->audio_samples,
                                                       AUDIO_TEMP_FRAMES);
            const size_t pushed = audio_->pushRealtimeStereo(impl_->audio_samples, frames);
            impl_->audio_frames_pushed.fetch_add(static_cast<uint32_t>(pushed));
            if (pushed < frames) impl_->audio_frames_dropped.fetch_add(frames - pushed);
        }

        ++local_frame;
        int64_t remaining = next_frame_us - esp_timer_get_time();
        if (remaining > 2000) {
            vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(remaining / 1000)));
        }
        while (!stop_requested_.load() && esp_timer_get_time() < next_frame_us) {
            taskYIELD();
        }
        if (remaining <= 0 && (++impl_->behind_yield_counter & 0x1FU) == 0U) {
            vTaskDelay(1);
        }
        if (esp_timer_get_time() - next_frame_us > FRAME_PERIOD_US * 3) {
            next_frame_us = esp_timer_get_time();
        }

        if (audio_ && audio_->enabled()) {
            const audio::RealtimeInfo info = audio_->realtimeInfo();
            if (info.buffered_frames > AUDIO_TARGET_FRAMES) vTaskDelay(1);
        }
    }

    task_running_.store(false);
    vTaskDelete(nullptr);
}

bool GameBoyAdvance::drawLatest(gfx::Canvas& canvas, ScaleMode mode, bool force_full) {
    if (!impl_ || !loaded_.load()) return false;
    const uint32_t sequence = frame_sequence_.load(std::memory_order_acquire);
    if (!force_full && sequence == impl_->last_draw_sequence && impl_->previous_valid &&
        mode == impl_->previous_scale) {
        return false;
    }

    xSemaphoreTake(impl_->frame_mutex, portMAX_DELAY);
    std::memcpy(impl_->frame_render, impl_->frame_front, FRAME_BYTES);
    xSemaphoreGive(impl_->frame_mutex);

    if (force_full || !impl_->previous_valid || mode != impl_->previous_scale) {
        canvas.clear(gfx::WHITE);
    }
    int x = 80;
    int y = 40;
    int width = SCREEN_WIDTH;
    int height = SCREEN_HEIGHT;
    if (mode == ScaleMode::FitHeight) {
        x = 20;
        y = 0;
        width = 360;
        height = 240;
    }
    canvas.draw_rgb565_fast(x, y, SCREEN_WIDTH, SCREEN_HEIGHT,
                            impl_->frame_render, width, height, config_.dither);
    impl_->previous_valid = true;
    impl_->previous_scale = mode;
    impl_->last_draw_sequence = sequence;
    impl_->displayed_frames.fetch_add(1);
    return true;
}

const char* GameBoyAdvance::romTitle() const {
    return impl_ ? impl_->rom_title : "";
}

const char* GameBoyAdvance::romPath() const {
    return impl_ ? impl_->rom_path : "";
}

Stats GameBoyAdvance::stats() const {
    Stats result;
    if (!impl_) return result;
    result.emulated_frames = impl_->emulated_frames.load();
    result.rendered_frames = impl_->rendered_frames.load();
    result.displayed_frames = impl_->displayed_frames.load();
    result.audio_frames_pushed = impl_->audio_frames_pushed.load();
    result.audio_frames_dropped = impl_->audio_frames_dropped.load();
    result.page_loads = impl_->page_loads.load();
    result.page_load_us = impl_->page_load_us.load();
    result.save_writes = impl_->save_writes.load();
    result.last_frame_us = impl_->last_frame_us.load();
    result.max_frame_us = impl_->max_frame_us.load();
    result.rom_bytes = impl_->rom_size;
    result.rom_buffer_bytes = gamepak_buffer_count * 1024U * 1024U;
    result.frame_buffer_bytes = FRAME_BYTES * 3U;
    result.save_bytes = activeSaveSize();
    return result;
}

const char* scale_mode_name(ScaleMode mode) {
    return mode == ScaleMode::OneX ? "1X FAST" : "FIT 360";
}

} // namespace pogopo::gameboy_advance
