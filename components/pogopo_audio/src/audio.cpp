#include "pogopo/audio/audio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace pogopo::audio {
namespace {
constexpr char TAG[] = "pogopo_audio";
constexpr double TWO_PI = 6.28318530717958647692;
constexpr uint32_t WRITE_TIMEOUT_MS = 1000;

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8U));
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) |
           (static_cast<uint32_t>(p[3]) << 24U);
}
} // namespace

Audio::~Audio() {
    end();
}

esp_err_t Audio::begin(const Config& config) {
    if (ok_.load() || task_ || stream_task_ || queue_ || stream_queue_ || tx_channel_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config.dout_io < 0 || config.bclk_io < 0 || config.lrck_io < 0 ||
        config.sample_rate < 8000 || config.queue_depth == 0 ||
        config.dma_desc_num < 2 || config.dma_frame_num == 0 ||
        config.render_frames == 0 || config.render_frames > MAX_RENDER_FRAMES ||
        config.realtime_buffer_frames < 1024 ||
        config.stream_buffer_frames < 2048 || config.stream_queue_depth == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    config_ = config;
    enabled_.store(true);
    master_volume_.store(std::min<uint8_t>(config.master_volume, 100));
    active_voices_.store(0);
    buffers_written_.store(0);
    write_errors_.store(0);
    short_writes_.store(0);
    dropped_commands_.store(0);
    last_write_us_.store(0);
    max_write_us_.store(0);
    voice_serial_ = 0;
    noise_state_ = 0xA5C31E27u;
    silenceVoices();
    clearPcm();
    clearMusicPcm();
    initSineTable();

    realtime_capacity_frames_ = config_.realtime_buffer_frames;
    // Emulator audio is touched continuously by two cores. Keep its compact
    // ring in internal RAM first; PSRAM is only a fallback.
    realtime_buffer_ = static_cast<int16_t*>(heap_caps_malloc(
        static_cast<size_t>(realtime_capacity_frames_) * 2U * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!realtime_buffer_) {
        realtime_buffer_ = static_cast<int16_t*>(heap_caps_malloc(
            static_cast<size_t>(realtime_capacity_frames_) * 2U * sizeof(int16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!realtime_buffer_) {
        return ESP_ERR_NO_MEM;
    }
    std::memset(realtime_buffer_, 0,
                static_cast<size_t>(realtime_capacity_frames_) * 2U * sizeof(int16_t));
    stopRealtime();

    stream_buffer_ = static_cast<int16_t*>(heap_caps_malloc(
        static_cast<size_t>(config_.stream_buffer_frames) * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!stream_buffer_) {
        stream_buffer_ = static_cast<int16_t*>(heap_caps_malloc(
            static_cast<size_t>(config_.stream_buffer_frames) * sizeof(int16_t),
            MALLOC_CAP_8BIT));
    }
    if (!stream_buffer_) {
        cleanupStreamResources();
        return ESP_ERR_NO_MEM;
    }
    std::memset(stream_buffer_, 0,
                static_cast<size_t>(config_.stream_buffer_frames) * sizeof(int16_t));

    queue_ = xQueueCreate(config_.queue_depth, sizeof(Command));
    stream_queue_ = xQueueCreate(config_.stream_queue_depth, sizeof(StreamCommand));
    if (!queue_ || !stream_queue_) {
        cleanupStreamResources();
        if (queue_) {
            vQueueDelete(queue_);
            queue_ = nullptr;
        }
        if (stream_queue_) {
            vQueueDelete(stream_queue_);
            stream_queue_ = nullptr;
        }
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = config_.dma_desc_num;
    channel_config.dma_frame_num = config_.dma_frame_num;
    channel_config.auto_clear_after_cb = true;
    channel_config.auto_clear_before_cb = false;

    esp_err_t err = i2s_new_channel(&channel_config, &tx_channel_, nullptr);
    if (err != ESP_OK) {
        end();
        return err;
    }

    i2s_std_clk_config_t clock_config = I2S_STD_CLK_DEFAULT_CONFIG(config_.sample_rate);
    i2s_std_slot_config_t slot_config =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    i2s_std_gpio_config_t gpio_config = {};
    gpio_config.mclk = I2S_GPIO_UNUSED;
    gpio_config.bclk = static_cast<gpio_num_t>(config_.bclk_io);
    gpio_config.ws = static_cast<gpio_num_t>(config_.lrck_io);
    gpio_config.dout = static_cast<gpio_num_t>(config_.dout_io);
    gpio_config.din = I2S_GPIO_UNUSED;
    gpio_config.invert_flags.mclk_inv = false;
    gpio_config.invert_flags.bclk_inv = false;
    gpio_config.invert_flags.ws_inv = false;

    i2s_std_config_t standard_config = {};
    standard_config.clk_cfg = clock_config;
    standard_config.slot_cfg = slot_config;
    standard_config.gpio_cfg = gpio_config;

    err = i2s_channel_init_std_mode(tx_channel_, &standard_config);
    if (err != ESP_OK) {
        end();
        return err;
    }

    std::array<int16_t, 128 * 2> silence{};
    size_t bytes_loaded = 0;
    i2s_channel_preload_data(tx_channel_, silence.data(), sizeof(silence), &bytes_loaded);

    err = i2s_channel_enable(tx_channel_);
    if (err != ESP_OK) {
        end();
        return err;
    }

    stream_exit_.store(false);
    const BaseType_t stream_created = xTaskCreatePinnedToCore(
        stream_task_entry,
        "pogopo_stream",
        config_.stream_task_stack,
        this,
        config_.stream_task_priority,
        &stream_task_,
        config_.stream_task_core);
    if (stream_created != pdPASS) {
        end();
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        task_entry,
        "pogopo_audio",
        config_.task_stack,
        this,
        config_.task_priority,
        &task_,
        config_.task_core);
    if (created != pdPASS) {
        end();
        return ESP_ERR_NO_MEM;
    }

    ok_.store(true);
    ESP_LOGI(TAG,
             "Audio ready: %lu Hz, realtime=%lu stereo frames, WAV stream=%lu mono frames",
             static_cast<unsigned long>(config_.sample_rate),
             static_cast<unsigned long>(realtime_capacity_frames_),
             static_cast<unsigned long>(config_.stream_buffer_frames));
    return ESP_OK;
}

void Audio::end() {
    ok_.store(false);
    stream_exit_.store(true);

    if (task_) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (stream_task_) {
        vTaskDelete(stream_task_);
        stream_task_ = nullptr;
    }

    closeStreamFile();

    if (queue_) {
        Command pending;
        while (xQueueReceive(queue_, &pending, 0) == pdTRUE) {
            if ((pending.type == CommandType::PlayPcm ||
                 pending.type == CommandType::PlayMusicPcm) && pending.pcm_samples) {
                heap_caps_free(pending.pcm_samples);
            }
        }
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
    if (stream_queue_) {
        vQueueDelete(stream_queue_);
        stream_queue_ = nullptr;
    }

    silenceVoices();
    clearPcm();
    clearMusicPcm();
    active_voices_.store(0);
    cleanupI2s();
    cleanupStreamResources();
}

bool Audio::play(Effect effect) {
    if (!enabled_.load()) {
        return true;
    }
    Command command;
    command.type = CommandType::PlayEffect;
    command.effect = effect;
    return enqueue(command);
}

bool Audio::tone(uint16_t frequency_hz, uint16_t duration_ms,
                 uint8_t volume, Waveform waveform) {
    if (!enabled_.load()) {
        return true;
    }
    if (frequency_hz < 20 || duration_ms == 0) {
        return false;
    }
    const uint32_t sample_rate = config_.sample_rate ? config_.sample_rate : 32768;
    if (frequency_hz >= sample_rate / 2U) {
        return false;
    }

    Command command;
    command.type = CommandType::PlayTone;
    command.waveform = waveform;
    command.frequency_hz = frequency_hz;
    command.duration_ms = duration_ms;
    command.volume = std::min<uint8_t>(volume, 100);
    return enqueue(command);
}

uint32_t Audio::playSynthTone(uint16_t frequency_hz, uint16_t duration_ms,
                              uint8_t volume, Waveform waveform,
                              uint16_t attack_ms, uint16_t decay_ms,
                              uint16_t sustain_q15, uint16_t release_ms) {
    if (!enabled_.load()) {
        return 1;
    }
    const uint32_t sample_rate = config_.sample_rate ? config_.sample_rate : 32768;
    if (frequency_hz < 20 || frequency_hz >= sample_rate / 2U ||
        duration_ms == 0) {
        return 0;
    }

    uint32_t token = next_synth_token_.fetch_add(1);
    if (token == 0) token = next_synth_token_.fetch_add(1);
    Command command;
    command.type = CommandType::PlayTone;
    command.waveform = waveform;
    command.frequency_hz = frequency_hz;
    command.duration_ms = duration_ms;
    command.volume = std::min<uint8_t>(volume, 100);
    command.attack_ms = std::min<uint16_t>(attack_ms, duration_ms);
    command.decay_ms = std::min<uint16_t>(decay_ms, duration_ms);
    command.sustain_q15 = std::min<uint16_t>(sustain_q15, 32767);
    command.release_ms = std::min<uint16_t>(release_ms, duration_ms);
    command.synth_token = token;
    return enqueue(command) ? token : 0;
}

bool Audio::stopSynthTone(uint32_t token, uint16_t release_ms) {
    if (token == 0) return false;
    Command command;
    command.type = CommandType::StopSynthTone;
    command.synth_token = token;
    command.release_ms = release_ms;
    return enqueue(command);
}

bool Audio::playPcmOwned(int16_t* samples, uint32_t frames, uint32_t sample_rate, uint8_t volume) {
    if (!samples || frames == 0 || sample_rate < 8000 || sample_rate > 96000) {
        if (samples) {
            heap_caps_free(samples);
        }
        return false;
    }
    if (!enabled_.load()) {
        heap_caps_free(samples);
        return true;
    }

    Command command;
    command.type = CommandType::PlayPcm;
    command.volume = std::min<uint8_t>(volume, 100);
    command.pcm_samples = samples;
    command.pcm_frames = frames;
    command.pcm_sample_rate = sample_rate;
    if (!enqueue(command)) {
        heap_caps_free(samples);
        return false;
    }
    return true;
}

bool Audio::playMusicPcmOwned(int16_t* samples, uint32_t frames,
                              uint32_t sample_rate, uint8_t volume, bool loop) {
    if (!samples || frames == 0 || sample_rate < 8000 || sample_rate > 96000) {
        if (samples) heap_caps_free(samples);
        return false;
    }
    if (!enabled_.load()) {
        heap_caps_free(samples);
        return true;
    }
    Command command;
    command.type = CommandType::PlayMusicPcm;
    command.volume = std::min<uint8_t>(volume, 100);
    command.pcm_samples = samples;
    command.pcm_frames = frames;
    command.pcm_sample_rate = sample_rate;
    command.pcm_loop = loop;
    if (!enqueue(command)) {
        heap_caps_free(samples);
        return false;
    }
    return true;
}

void Audio::stopMusicPcm() {
    Command command;
    command.type = CommandType::StopMusicPcm;
    enqueue(command);
}

void Audio::stopAll() {
    stopRealtime();
    Command command;
    command.type = CommandType::StopAll;
    enqueue(command);
    stopStream();
}

esp_err_t Audio::startRealtimeStereo(uint32_t sample_rate, uint8_t volume) {
    if (!ok_.load() || !realtime_buffer_ || realtime_capacity_frames_ == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate < 8000U || sample_rate > 96000U || config_.sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    realtime_active_.store(false);
    realtime_write_total_.store(0);
    realtime_read_total_.store(0);
    realtime_underruns_.store(0);
    realtime_overruns_.store(0);
    realtime_volume_.store(std::min<uint8_t>(volume, 100));
    realtime_source_rate_.store(sample_rate);
    realtime_fraction_q16_ = 0;
    realtime_step_q16_ = static_cast<uint32_t>(std::max<uint64_t>(
        1U, (static_cast<uint64_t>(sample_rate) << 16U) / config_.sample_rate));
    realtime_last_left_ = 0;
    realtime_last_right_ = 0;
    realtime_fade_samples_ = 0;
    realtime_fade_in_samples_ = 0;
    realtime_underrun_latched_ = false;
    std::memset(realtime_buffer_, 0,
                static_cast<size_t>(realtime_capacity_frames_) * 2U * sizeof(int16_t));
    realtime_active_.store(true);
    return ESP_OK;
}

void Audio::stopRealtime() {
    realtime_active_.store(false);
    realtime_write_total_.store(0);
    realtime_read_total_.store(0);
    realtime_fraction_q16_ = 0;
    realtime_step_q16_ = 1U << 16U;
    realtime_last_left_ = 0;
    realtime_last_right_ = 0;
    realtime_fade_samples_ = 0;
    realtime_fade_in_samples_ = 0;
    realtime_underrun_latched_ = false;
}

size_t Audio::pushRealtimeStereo(const int16_t* interleaved_stereo, size_t frames) {
    if (!interleaved_stereo || frames == 0 || !realtime_active_.load() ||
        !realtime_buffer_ || realtime_capacity_frames_ == 0 || !enabled_.load()) {
        return 0;
    }

    const uint32_t read = realtime_read_total_.load(std::memory_order_acquire);
    const uint32_t write = realtime_write_total_.load(std::memory_order_relaxed);
    const uint32_t used = write - read;
    const uint32_t free_frames = used < realtime_capacity_frames_
        ? realtime_capacity_frames_ - used
        : 0;
    // Never write half of a Game Boy frame packet. A partial packet creates a
    // discontinuity in the middle of the stream and is heard as a sharp crack.
    // Like the stable Arduino queue, drop the complete newest packet instead.
    if (frames > free_frames) {
        realtime_overruns_.fetch_add(static_cast<uint32_t>(frames));
        return 0;
    }

    // Copy at most two contiguous spans instead of doing a modulo operation
    // for every stereo frame. This producer runs on the same core as I2S.
    const uint32_t slot = write % realtime_capacity_frames_;
    const size_t first_frames = std::min<size_t>(
        frames, static_cast<size_t>(realtime_capacity_frames_ - slot));
    std::memcpy(realtime_buffer_ + static_cast<size_t>(slot) * 2U,
                interleaved_stereo, first_frames * 2U * sizeof(int16_t));
    const size_t second_frames = frames - first_frames;
    if (second_frames > 0) {
        std::memcpy(realtime_buffer_, interleaved_stereo + first_frames * 2U,
                    second_frames * 2U * sizeof(int16_t));
    }
    realtime_write_total_.store(write + static_cast<uint32_t>(frames),
                                std::memory_order_release);
    return frames;
}

RealtimeInfo Audio::realtimeInfo() const {
    RealtimeInfo info;
    info.active = realtime_active_.load();
    const uint32_t write = realtime_write_total_.load();
    const uint32_t read = realtime_read_total_.load();
    info.buffered_frames = write - read;
    info.capacity_frames = realtime_capacity_frames_;
    info.source_rate = realtime_source_rate_.load();
    info.underruns = realtime_underruns_.load();
    info.overruns = realtime_overruns_.load();
    info.volume = realtime_volume_.load();
    return info;
}

bool Audio::playStream(const char* path, uint8_t volume) {
    if (!enabled_.load() || !ok_.load() || !path) {
        return false;
    }
    const size_t length = std::strlen(path);
    if (length == 0 || length >= STREAM_PATH_SIZE) {
        return false;
    }

    StreamCommand command;
    command.type = StreamCommandType::Play;
    command.volume = std::min<uint8_t>(volume, 100);
    std::memcpy(command.path, path, length + 1U);
    const StreamState previous = streamState();
    stream_state_.store(static_cast<uint8_t>(StreamState::Opening));
    if (!enqueueStream(command)) {
        stream_state_.store(static_cast<uint8_t>(previous));
        return false;
    }
    return true;
}

bool Audio::pauseStream() {
    StreamCommand command;
    command.type = StreamCommandType::Pause;
    return enqueueStream(command);
}

bool Audio::resumeStream() {
    StreamCommand command;
    command.type = StreamCommandType::Resume;
    return enqueueStream(command);
}

bool Audio::toggleStreamPause() {
    const StreamState state = streamState();
    if (state == StreamState::Paused) {
        return resumeStream();
    }
    if (state == StreamState::Playing || state == StreamState::Buffering) {
        return pauseStream();
    }
    return false;
}

bool Audio::stopStream() {
    if (!stream_queue_) {
        return false;
    }
    StreamCommand command;
    command.type = StreamCommandType::Stop;
    return enqueueStream(command);
}

bool Audio::seekStreamMs(uint32_t position_ms) {
    if (!streamActive() && streamState() != StreamState::Finished) {
        return false;
    }
    StreamCommand command;
    command.type = StreamCommandType::Seek;
    command.position_ms = position_ms;
    return enqueueStream(command);
}

StreamInfo Audio::streamInfo() const {
    StreamInfo info;
    info.state = streamState();
    info.position_ms = stream_position_ms_.load();
    info.duration_ms = stream_duration_ms_.load();
    info.buffered_ms = stream_buffered_ms_.load();
    info.sample_rate = stream_source_rate_.load();
    info.underruns = stream_underruns_.load();
    info.read_errors = stream_read_errors_.load();
    info.dropped_commands = stream_dropped_commands_.load();
    info.channels = stream_channels_.load();
    info.bits_per_sample = stream_bits_.load();
    info.volume = stream_volume_.load();
    return info;
}

StreamState Audio::streamState() const {
    return static_cast<StreamState>(stream_state_.load());
}

bool Audio::streamActive() const {
    const StreamState state = streamState();
    return state == StreamState::Opening || state == StreamState::Buffering ||
           state == StreamState::Playing || state == StreamState::Paused;
}

void Audio::setMasterVolume(uint8_t percent) {
    master_volume_.store(std::min<uint8_t>(percent, 100));
}

void Audio::setEnabled(bool enabled) {
    const bool previous = enabled_.exchange(enabled);
    if (previous && !enabled) {
        stopAll();
    }
}

Stats Audio::stats() const {
    Stats result;
    result.buffers_written = buffers_written_.load();
    result.write_errors = write_errors_.load();
    result.short_writes = short_writes_.load();
    result.dropped_commands = dropped_commands_.load();
    result.last_write_us = last_write_us_.load();
    result.max_write_us = max_write_us_.load();
    result.active_voices = active_voices_.load();
    return result;
}

bool Audio::enqueue(const Command& command) {
    if (!ok_.load() || !queue_) {
        return false;
    }
    if (xQueueSend(queue_, &command, 0) != pdTRUE) {
        dropped_commands_.fetch_add(1);
        return false;
    }
    return true;
}

bool Audio::enqueueStream(const StreamCommand& command) {
    if (!ok_.load() || !stream_queue_) {
        return false;
    }
    if (xQueueSend(stream_queue_, &command, 0) != pdTRUE) {
        stream_dropped_commands_.fetch_add(1);
        return false;
    }
    return true;
}

void Audio::task_entry(void* argument) {
    static_cast<Audio*>(argument)->task_loop();
}

void Audio::stream_task_entry(void* argument) {
    static_cast<Audio*>(argument)->stream_task_loop();
}

void Audio::task_loop() {
    std::array<int16_t, MAX_RENDER_FRAMES * 2> output{};

    while (true) {
        processCommands();

        const bool realtime_exclusive = realtime_active_.load(std::memory_order_acquire);
        if (realtime_exclusive) {
            // Game Boy owns the audio path. Do not spend 32768 iterations/s on
            // four synth voices, WAV streaming and the generic mixer.
            silenceVoices();
            clearPcm();
            clearMusicPcm();
            active_voices_.store(0);
            for (size_t frame = 0; frame < config_.render_frames; ++frame) {
                int32_t left = 0;
                int32_t right = 0;
                renderRealtime(left, right);
                output[frame * 2U] = static_cast<int16_t>(
                    std::clamp<int32_t>(left, -32767, 32767));
                output[frame * 2U + 1U] = static_cast<int16_t>(
                    std::clamp<int32_t>(right, -32767, 32767));
            }
        } else {
            for (size_t frame = 0; frame < config_.render_frames; ++frame) {
                int32_t mono = 0;
                for (auto& voice : voices_) {
                    mono += renderVoice(voice);
                }
                mono += renderPcm();
                mono += renderMusicPcm();
                mono += renderStream();

                int32_t realtime_left = 0;
                int32_t realtime_right = 0;
                renderRealtime(realtime_left, realtime_right);

                const int32_t left = std::clamp<int32_t>(mono + realtime_left, -32767, 32767);
                const int32_t right = std::clamp<int32_t>(mono + realtime_right, -32767, 32767);
                output[frame * 2U] = static_cast<int16_t>(left);
                output[frame * 2U + 1U] = static_cast<int16_t>(right);
            }
            updateActiveVoiceCount();
        }

        const size_t requested_bytes =
            static_cast<size_t>(config_.render_frames) * 2U * sizeof(int16_t);
        size_t written_bytes = 0;
        const int64_t start_us = esp_timer_get_time();
        const esp_err_t err = i2s_channel_write(
            tx_channel_, output.data(), requested_bytes, &written_bytes, WRITE_TIMEOUT_MS);
        const uint32_t elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - start_us);
        last_write_us_.store(elapsed_us);

        uint32_t observed_max = max_write_us_.load();
        while (elapsed_us > observed_max &&
               !max_write_us_.compare_exchange_weak(observed_max, elapsed_us)) {
        }

        if (err != ESP_OK) {
            write_errors_.fetch_add(1);
            ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(err));
        } else {
            buffers_written_.fetch_add(1);
            if (written_bytes != requested_bytes) {
                short_writes_.fetch_add(1);
            }
        }
    }
}

void Audio::stream_task_loop() {
    while (!stream_exit_.load()) {
        StreamCommand command;
        bool handled = false;
        while (xQueueReceive(stream_queue_, &command, 0) == pdTRUE) {
            handleStreamCommand(command);
            handled = true;
        }

        if (stream_file_ && !stream_eof_.load()) {
            fillStreamBuffer();
        } else if (!handled) {
            if (xQueueReceive(stream_queue_, &command, pdMS_TO_TICKS(10)) == pdTRUE) {
                handleStreamCommand(command);
            }
        }

        if (stream_file_ && !stream_eof_.load()) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    closeStreamFile();
    vTaskDelete(nullptr);
}

void Audio::processCommands() {
    Command command;
    while (xQueueReceive(queue_, &command, 0) == pdTRUE) {
        switch (command.type) {
            case CommandType::PlayEffect:
                startEffect(command.effect);
                break;
            case CommandType::PlayTone:
                startTone(command);
                break;
            case CommandType::PlayPcm:
                startPcm(command);
                break;
            case CommandType::PlayMusicPcm:
                startMusicPcm(command);
                break;
            case CommandType::StopMusicPcm:
                clearMusicPcm();
                break;
            case CommandType::StopSynthTone:
                stopTone(command);
                break;
            case CommandType::StopAll:
                silenceVoices();
                clearPcm();
                clearMusicPcm();
                break;
        }
    }
    updateActiveVoiceCount();
}

Audio::Voice& Audio::chooseVoice(bool custom, Effect effect) {
    if (!custom) {
        for (auto& voice : voices_) {
            if (voice.active && !voice.custom && voice.effect == effect) {
                voice = Voice{};
                voice.serial = ++voice_serial_;
                return voice;
            }
        }
    }

    for (auto& voice : voices_) {
        if (!voice.active) {
            voice = Voice{};
            voice.serial = ++voice_serial_;
            return voice;
        }
    }

    auto* oldest = &voices_[0];
    for (auto& voice : voices_) {
        if (voice.serial < oldest->serial) {
            oldest = &voice;
        }
    }
    *oldest = Voice{};
    oldest->serial = ++voice_serial_;
    return *oldest;
}

void Audio::startEffect(Effect effect) {
    size_t count = 0;
    const Note* notes = patternForEffect(effect, count);
    if (!notes || count == 0) {
        return;
    }

    Voice& voice = chooseVoice(false, effect);
    voice.active = true;
    voice.custom = false;
    voice.effect = effect;
    voice.notes = notes;
    voice.note_count = count;
    voice.note_index = 0;
    voice.phase = 0;
    loadCurrentNote(voice);
}

void Audio::startTone(const Command& command) {
    Voice& voice = chooseVoice(true, Effect::Click);
    voice.active = true;
    voice.custom = true;
    voice.custom_note = {
        command.frequency_hz,
        command.duration_ms,
        command.volume,
        command.waveform,
        command.attack_ms,
        command.release_ms,
        command.decay_ms,
        command.sustain_q15,
    };
    voice.notes = &voice.custom_note;
    voice.note_count = 1;
    voice.note_index = 0;
    voice.phase = 0;
    voice.synth_token = command.synth_token;
    loadCurrentNote(voice);
}

void Audio::stopTone(const Command& command) {
    for (auto& voice : voices_) {
        if (!voice.active || voice.synth_token != command.synth_token) continue;
        if (command.release_ms == 0) {
            voice = Voice{};
        } else {
            voice.release_samples = static_cast<uint32_t>(
                (static_cast<uint64_t>(command.release_ms) * config_.sample_rate) /
                1000U);
            voice.release_samples = std::max<uint32_t>(1U, voice.release_samples);
            voice.release_samples = std::min(
                voice.release_samples, voice.samples_total);
            voice.samples_left = std::min(voice.samples_left, voice.release_samples);
        }
        break;
    }
}

void Audio::startPcm(Command& command) {
    clearPcm();
    pcm_.samples = command.pcm_samples;
    pcm_.frames = command.pcm_frames;
    pcm_.position_q16 = 0;
    pcm_.step_q16 =
        (static_cast<uint64_t>(command.pcm_sample_rate) << 16U) / config_.sample_rate;
    pcm_.step_q16 = std::max<uint64_t>(1U, pcm_.step_q16);
    pcm_.volume = command.volume;
    pcm_.active = true;
    pcm_active_.store(true);
    command.pcm_samples = nullptr;
}

void Audio::startMusicPcm(Command& command) {
    clearMusicPcm();
    music_pcm_.samples = command.pcm_samples;
    music_pcm_.frames = command.pcm_frames;
    music_pcm_.position_q16 = 0;
    music_pcm_.step_q16 =
        (static_cast<uint64_t>(command.pcm_sample_rate) << 16U) / config_.sample_rate;
    music_pcm_.step_q16 = std::max<uint64_t>(1U, music_pcm_.step_q16);
    music_pcm_.volume = command.volume;
    music_pcm_.loop = command.pcm_loop;
    music_pcm_.active = true;
    music_pcm_active_.store(true);
    command.pcm_samples = nullptr;
}

int32_t Audio::renderPcm() {
    if (!pcm_.active || !pcm_.samples || pcm_.frames == 0 || !enabled_.load()) {
        return 0;
    }
    const uint32_t index = static_cast<uint32_t>(pcm_.position_q16 >> 16U);
    if (index >= pcm_.frames) {
        clearPcm();
        return 0;
    }
    const int32_t raw = pcm_.samples[index];
    const int32_t sample = static_cast<int32_t>(
        (static_cast<int64_t>(raw) * pcm_.volume * master_volume_.load()) / 10000LL);
    pcm_.position_q16 += pcm_.step_q16;
    if ((pcm_.position_q16 >> 16U) >= pcm_.frames) {
        clearPcm();
    }
    return sample;
}

int32_t Audio::renderMusicPcm() {
    if (!music_pcm_.active || !music_pcm_.samples || music_pcm_.frames == 0 ||
        !enabled_.load()) return 0;
    uint32_t index = static_cast<uint32_t>(music_pcm_.position_q16 >> 16U);
    if (index >= music_pcm_.frames) {
        if (!music_pcm_.loop) {
            clearMusicPcm();
            return 0;
        }
        music_pcm_.position_q16 %= static_cast<uint64_t>(music_pcm_.frames) << 16U;
        index = static_cast<uint32_t>(music_pcm_.position_q16 >> 16U);
    }
    const int32_t raw = music_pcm_.samples[index];
    const int32_t sample = static_cast<int32_t>(
        (static_cast<int64_t>(raw) * music_pcm_.volume * master_volume_.load()) / 10000LL);
    music_pcm_.position_q16 += music_pcm_.step_q16;
    return sample;
}

int32_t Audio::renderStream() {
    if (!enabled_.load() || !stream_buffer_ || stream_resetting_.load()) {
        return 0;
    }
    if (streamState() != StreamState::Playing) {
        return 0;
    }

    const uint32_t generation = stream_generation_.load();
    if (generation != stream_generation_seen_) {
        stream_generation_seen_ = generation;
        stream_position_q16_ = 0;
        const uint32_t source_rate = stream_source_rate_.load();
        stream_step_q16_ = source_rate
            ? (static_cast<uint64_t>(source_rate) << 16U) / config_.sample_rate
            : 0;
        stream_step_q16_ = std::max<uint64_t>(1U, stream_step_q16_);
        stream_underrun_latched_ = false;
    }

    const uint32_t source_rate = stream_source_rate_.load();
    if (source_rate == 0 || stream_step_q16_ == 0) {
        return 0;
    }

    const uint32_t write_total = stream_write_total_.load();
    const uint32_t index = static_cast<uint32_t>(stream_position_q16_ >> 16U);
    const bool eof = stream_eof_.load();

    if (index >= write_total) {
        if (eof) {
            stream_state_.store(static_cast<uint8_t>(StreamState::Finished));
            stream_position_ms_.store(stream_duration_ms_.load());
        } else {
            if (!stream_underrun_latched_) {
                stream_underruns_.fetch_add(1);
                stream_underrun_latched_ = true;
            }
            stream_state_.store(static_cast<uint8_t>(StreamState::Buffering));
        }
        return 0;
    }

    if (index + 1U >= write_total && !eof) {
        if (!stream_underrun_latched_) {
            stream_underruns_.fetch_add(1);
            stream_underrun_latched_ = true;
        }
        stream_state_.store(static_cast<uint8_t>(StreamState::Buffering));
        return 0;
    }

    stream_underrun_latched_ = false;
    const uint32_t capacity = config_.stream_buffer_frames;
    const int32_t first = stream_buffer_[index % capacity];
    const int32_t second = (index + 1U < write_total)
        ? stream_buffer_[(index + 1U) % capacity]
        : first;
    const uint32_t fraction = static_cast<uint32_t>(stream_position_q16_ & 0xFFFFU);
    const int32_t interpolated = static_cast<int32_t>(
        (static_cast<int64_t>(first) * (65536U - fraction) +
         static_cast<int64_t>(second) * fraction) >> 16U);

    const int32_t sample = static_cast<int32_t>(
        (static_cast<int64_t>(interpolated) * stream_volume_.load() *
         master_volume_.load()) / 10000LL);

    stream_position_q16_ += stream_step_q16_;
    const uint32_t consumed = static_cast<uint32_t>(stream_position_q16_ >> 16U);
    stream_consumed_total_.store(std::min(consumed, write_total));

    const uint32_t base = stream_base_frame_.load();
    const uint32_t total = stream_total_frames_.load();
    const uint32_t absolute_frame = std::min<uint32_t>(base + consumed, total);
    stream_position_ms_.store(static_cast<uint32_t>(
        (static_cast<uint64_t>(absolute_frame) * 1000U) / source_rate));

    const uint32_t buffered_frames = write_total > consumed ? write_total - consumed : 0;
    stream_buffered_ms_.store(static_cast<uint32_t>(
        (static_cast<uint64_t>(buffered_frames) * 1000U) / source_rate));

    if (eof && consumed >= write_total) {
        stream_state_.store(static_cast<uint8_t>(StreamState::Finished));
        stream_position_ms_.store(stream_duration_ms_.load());
        stream_buffered_ms_.store(0);
    }
    return sample;
}

void Audio::renderRealtime(int32_t& left, int32_t& right) {
    left = 0;
    right = 0;
    if (!realtime_active_.load() || !realtime_buffer_ || !enabled_.load()) {
        return;
    }

    const uint32_t read = realtime_read_total_.load(std::memory_order_relaxed);
    const uint32_t write = realtime_write_total_.load(std::memory_order_acquire);
    const uint32_t available = write - read;
    if (available == 0) {
        if (!realtime_underrun_latched_) {
            realtime_underruns_.fetch_add(1);
            realtime_underrun_latched_ = true;
        }
        // A short fade-to-zero is much less audible than an abrupt cut from an
        // arbitrary PCM sample to digital silence.
        if (realtime_fade_samples_ > 0) {
            left = static_cast<int32_t>(
                (static_cast<int64_t>(realtime_last_left_) * realtime_fade_samples_) / 64);
            right = static_cast<int32_t>(
                (static_cast<int64_t>(realtime_last_right_) * realtime_fade_samples_) / 64);
            --realtime_fade_samples_;
        }
        return;
    }

    if (realtime_underrun_latched_) {
        // The fixed-rate stream resumes from silence instead of jumping to an
        // arbitrary full-scale sample after an underrun.
        realtime_fade_in_samples_ = 64;
        realtime_underrun_latched_ = false;
    }
    const uint32_t first_slot = read % realtime_capacity_frames_;
    const int32_t scale = static_cast<int32_t>(realtime_volume_.load()) *
                          static_cast<int32_t>(master_volume_.load());

    const uint32_t playback_step_q16 = realtime_step_q16_;
    const auto apply_recovery_fade = [this](int32_t& sample_left, int32_t& sample_right) {
        if (realtime_fade_in_samples_ == 0) return;
        const uint16_t gain = static_cast<uint16_t>(64U - realtime_fade_in_samples_);
        sample_left = static_cast<int32_t>(
            (static_cast<int64_t>(sample_left) * gain) / 64);
        sample_right = static_cast<int32_t>(
            (static_cast<int64_t>(sample_right) * gain) / 64);
        --realtime_fade_in_samples_;
    };

    // The normal Game Boy path is 32768 -> 32768. Avoid interpolation, a second
    // ring read and 64-bit blend math while the emulator is keeping real time.
    if (playback_step_q16 == (1U << 16U)) {
        const int32_t raw_left = realtime_buffer_[first_slot * 2U];
        const int32_t raw_right = realtime_buffer_[first_slot * 2U + 1U];
        left = static_cast<int32_t>((static_cast<int64_t>(raw_left) * scale) / 10000LL);
        right = static_cast<int32_t>((static_cast<int64_t>(raw_right) * scale) / 10000LL);
        apply_recovery_fade(left, right);
        realtime_last_left_ = left;
        realtime_last_right_ = right;
        realtime_fade_samples_ = 64;
        realtime_read_total_.store(read + 1U, std::memory_order_release);
        return;
    }

    const uint32_t second_slot = available > 1U
        ? (read + 1U) % realtime_capacity_frames_
        : first_slot;
    const uint32_t fraction = realtime_fraction_q16_;
    const int32_t first_left = realtime_buffer_[first_slot * 2U];
    const int32_t first_right = realtime_buffer_[first_slot * 2U + 1U];
    const int32_t second_left = realtime_buffer_[second_slot * 2U];
    const int32_t second_right = realtime_buffer_[second_slot * 2U + 1U];
    const int32_t raw_left = static_cast<int32_t>(
        (static_cast<int64_t>(first_left) * (65536U - fraction) +
         static_cast<int64_t>(second_left) * fraction) >> 16U);
    const int32_t raw_right = static_cast<int32_t>(
        (static_cast<int64_t>(first_right) * (65536U - fraction) +
         static_cast<int64_t>(second_right) * fraction) >> 16U);
    left = static_cast<int32_t>((static_cast<int64_t>(raw_left) * scale) / 10000LL);
    right = static_cast<int32_t>((static_cast<int64_t>(raw_right) * scale) / 10000LL);
    apply_recovery_fade(left, right);

    realtime_last_left_ = left;
    realtime_last_right_ = right;
    realtime_fade_samples_ = 64;

    const uint32_t advanced_q16 = realtime_fraction_q16_ + playback_step_q16;
    uint32_t advance = advanced_q16 >> 16U;
    realtime_fraction_q16_ = advanced_q16 & 0xFFFFU;
    advance = std::min<uint32_t>(advance, available);
    realtime_read_total_.store(read + advance, std::memory_order_release);
}

void Audio::clearPcm() {
    if (pcm_.samples) {
        heap_caps_free(pcm_.samples);
    }
    pcm_ = {};
    pcm_active_.store(false);
}

void Audio::clearMusicPcm() {
    if (music_pcm_.samples) heap_caps_free(music_pcm_.samples);
    music_pcm_ = {};
    music_pcm_active_.store(false);
}

void Audio::loadCurrentNote(Voice& voice) {
    if (!voice.active || !voice.notes || voice.note_index >= voice.note_count) {
        voice.active = false;
        voice.samples_left = 0;
        return;
    }

    const Note& note = voice.notes[voice.note_index];
    voice.samples_total = std::max<uint32_t>(
        1U,
        static_cast<uint32_t>(
            (static_cast<uint64_t>(note.duration_ms) * config_.sample_rate) / 1000U));
    voice.samples_left = voice.samples_total;
    voice.attack_samples = static_cast<uint32_t>(
        (static_cast<uint64_t>(note.attack_ms) * config_.sample_rate) / 1000U);
    voice.decay_samples = static_cast<uint32_t>(
        (static_cast<uint64_t>(note.decay_ms) * config_.sample_rate) / 1000U);
    voice.sustain_q15 = std::min<uint16_t>(note.sustain_q15, 32767);
    voice.release_samples = static_cast<uint32_t>(
        (static_cast<uint64_t>(note.release_ms) * config_.sample_rate) / 1000U);
    voice.attack_samples = std::min(voice.attack_samples, voice.samples_total);
    voice.decay_samples = std::min(
        voice.decay_samples, voice.samples_total - voice.attack_samples);
    voice.release_samples = std::min(voice.release_samples, voice.samples_total);

    if (note.frequency_hz == 0) {
        voice.phase_increment = 0;
    } else {
        voice.phase_increment = static_cast<uint32_t>(
            (static_cast<uint64_t>(note.frequency_hz) << 32U) / config_.sample_rate);
    }
}

void Audio::advanceVoice(Voice& voice) {
    ++voice.note_index;
    if (voice.note_index >= voice.note_count) {
        voice.active = false;
        voice.samples_left = 0;
        return;
    }
    loadCurrentNote(voice);
}

int32_t Audio::renderVoice(Voice& voice) {
    if (!voice.active || !voice.notes || voice.samples_left == 0 || !enabled_.load()) {
        return 0;
    }
    const Note& note = voice.notes[voice.note_index];

    const int32_t raw = note.frequency_hz == 0 ? 0 : waveformSample(voice);
    const uint16_t envelope = envelopeQ15(voice);
    const uint32_t master = master_volume_.load();
    const int64_t scaled = static_cast<int64_t>(raw) * note.volume * master * envelope;
    const int32_t sample = static_cast<int32_t>(scaled / (100LL * 100LL * 32767LL));

    if (voice.samples_left > 0) {
        --voice.samples_left;
    }
    if (voice.samples_left == 0) {
        advanceVoice(voice);
    }
    return sample;
}

int16_t Audio::waveformSample(Voice& voice) {
    const Note& note = voice.notes[voice.note_index];
    int32_t value = 0;

    switch (note.waveform) {
        case Waveform::Sine: {
            const uint8_t index = static_cast<uint8_t>(voice.phase >> 24U);
            value = sine_table_[index];
            break;
        }
        case Waveform::Square:
            value = (voice.phase & 0x80000000U) ? -32767 : 32767;
            break;
        case Waveform::Triangle: {
            const uint32_t position = voice.phase >> 16U;
            value = position < 32768U
                ? static_cast<int32_t>(position * 2U) - 32768
                : 98303 - static_cast<int32_t>(position * 2U);
            break;
        }
        case Waveform::Noise:
            noise_state_ ^= noise_state_ << 13U;
            noise_state_ ^= noise_state_ >> 17U;
            noise_state_ ^= noise_state_ << 5U;
            value = static_cast<int16_t>(noise_state_ & 0xFFFFU);
            break;
        case Waveform::Sawtooth:
            value = static_cast<int32_t>(voice.phase >> 16U) - 32768;
            break;
    }

    voice.phase += voice.phase_increment;
    return static_cast<int16_t>(std::clamp<int32_t>(value, -32767, 32767));
}

uint16_t Audio::envelopeQ15(const Voice& voice) const {
    if (voice.samples_total == 0) {
        return 0;
    }
    const uint32_t elapsed = voice.samples_total - voice.samples_left;
    uint32_t envelope = 32767U;

    if (voice.attack_samples > 0 && elapsed < voice.attack_samples) {
        envelope = (elapsed * 32767U) / voice.attack_samples;
    } else if (voice.decay_samples > 0 &&
               elapsed < voice.attack_samples + voice.decay_samples) {
        const uint32_t decay_elapsed = elapsed - voice.attack_samples;
        const uint32_t range = 32767U - voice.sustain_q15;
        envelope = 32767U -
            (range * decay_elapsed) / voice.decay_samples;
    } else {
        envelope = voice.sustain_q15;
    }
    if (voice.release_samples > 0 && voice.samples_left < voice.release_samples) {
        const uint32_t release = (voice.samples_left * 32767U) / voice.release_samples;
        envelope = (envelope * release) / 32767U;
    }
    return static_cast<uint16_t>(envelope);
}

const Audio::Note* Audio::patternForEffect(Effect effect, size_t& count) const {
    static constexpr Note tick[] = {
        // Softer cursor feedback: lower pitch, triangle harmonics, less gain,
        // and a longer release than the old 1.9 kHz square spike.
        {1150, 22, 14, Waveform::Triangle, 3, 9},
    };
    static constexpr Note click[] = {
        {1250, 34, 58, Waveform::Triangle, 2, 7},
    };
    static constexpr Note confirm[] = {
        {620, 38, 62, Waveform::Sine, 3, 7},
        {0, 16, 0, Waveform::Sine, 0, 0},
        {930, 82, 72, Waveform::Sine, 3, 12},
    };
    static constexpr Note back[] = {
        {650, 38, 58, Waveform::Triangle, 2, 7},
        {0, 12, 0, Waveform::Sine, 0, 0},
        {360, 72, 62, Waveform::Triangle, 2, 12},
    };
    static constexpr Note error[] = {
        {190, 92, 40, Waveform::Square, 3, 12},
        {0, 34, 0, Waveform::Sine, 0, 0},
        {145, 125, 42, Waveform::Square, 3, 18},
    };
    static constexpr Note startup[] = {
        {523, 72, 58, Waveform::Sine, 4, 10},
        {0, 14, 0, Waveform::Sine, 0, 0},
        {659, 72, 62, Waveform::Sine, 4, 10},
        {0, 14, 0, Waveform::Sine, 0, 0},
        {784, 135, 72, Waveform::Sine, 4, 20},
    };
    static constexpr Note coin[] = {
        {988, 36, 62, Waveform::Square, 2, 6},
        {0, 10, 0, Waveform::Sine, 0, 0},
        {1319, 92, 70, Waveform::Sine, 2, 14},
    };

    switch (effect) {
        case Effect::Tick:
            count = std::size(tick);
            return tick;
        case Effect::Click:
            count = std::size(click);
            return click;
        case Effect::Confirm:
            count = std::size(confirm);
            return confirm;
        case Effect::Back:
            count = std::size(back);
            return back;
        case Effect::Error:
            count = std::size(error);
            return error;
        case Effect::Startup:
            count = std::size(startup);
            return startup;
        case Effect::Coin:
            count = std::size(coin);
            return coin;
    }
    count = 0;
    return nullptr;
}

void Audio::silenceVoices() {
    for (auto& voice : voices_) {
        voice = Voice{};
    }
    active_voices_.store(0);
}

void Audio::updateActiveVoiceCount() {
    uint8_t count = 0;
    for (const auto& voice : voices_) {
        if (voice.active) {
            ++count;
        }
    }
    if (pcm_.active) {
        ++count;
    }
    if (streamActive()) {
        ++count;
    }
    active_voices_.store(count);
}

void Audio::handleStreamCommand(const StreamCommand& command) {
    switch (command.type) {
        case StreamCommandType::Play:
            openStreamFile(command);
            break;
        case StreamCommandType::Pause:
            if (streamActive()) {
                stream_paused_.store(true);
                stream_state_.store(static_cast<uint8_t>(StreamState::Paused));
            }
            break;
        case StreamCommandType::Resume:
            if (streamState() == StreamState::Paused) {
                stream_paused_.store(false);
                updateStreamBufferingState();
            }
            break;
        case StreamCommandType::Stop:
            closeStreamFile();
            stream_paused_.store(false);
            resetStreamBuffer(0, false);
            stream_source_rate_.store(0);
            stream_channels_.store(0);
            stream_bits_.store(0);
            stream_total_frames_.store(0);
            stream_duration_ms_.store(0);
            stream_state_.store(static_cast<uint8_t>(StreamState::Stopped));
            break;
        case StreamCommandType::Seek: {
            const uint32_t rate = stream_source_rate_.load();
            const uint32_t total = stream_total_frames_.load();
            if (rate == 0 || total == 0 || !stream_file_) {
                break;
            }
            const uint32_t requested_frame = static_cast<uint32_t>(std::min<uint64_t>(
                (static_cast<uint64_t>(command.position_ms) * rate) / 1000U,
                total > 0 ? total - 1U : 0U));
            const long byte_offset = stream_header_.data_offset +
                static_cast<long>(requested_frame * stream_header_.block_align);
            if (std::fseek(stream_file_, byte_offset, SEEK_SET) != 0) {
                stream_read_errors_.fetch_add(1);
                stream_state_.store(static_cast<uint8_t>(StreamState::Error));
                break;
            }
            clearerr(stream_file_);
            stream_source_cursor_ = requested_frame;
            resetStreamBuffer(requested_frame, true);
            updateStreamBufferingState();
            break;
        }
    }
}

void Audio::openStreamFile(const StreamCommand& command) {
    closeStreamFile();
    stream_state_.store(static_cast<uint8_t>(StreamState::Opening));
    stream_paused_.store(false);
    stream_volume_.store(command.volume);

    FILE* file = std::fopen(command.path, "rb");
    if (!file) {
        stream_read_errors_.fetch_add(1);
        stream_state_.store(static_cast<uint8_t>(StreamState::Error));
        ESP_LOGW(TAG, "Could not open WAV stream: %s", command.path);
        return;
    }

    WavHeader header{};
    const esp_err_t parsed = parseWavHeader(file, header);
    if (parsed != ESP_OK) {
        std::fclose(file);
        stream_read_errors_.fetch_add(1);
        stream_state_.store(static_cast<uint8_t>(StreamState::Error));
        ESP_LOGW(TAG, "Unsupported WAV stream %s: %s", command.path, esp_err_to_name(parsed));
        return;
    }

    if (std::fseek(file, header.data_offset, SEEK_SET) != 0) {
        std::fclose(file);
        stream_read_errors_.fetch_add(1);
        stream_state_.store(static_cast<uint8_t>(StreamState::Error));
        return;
    }

    stream_file_ = file;
    stream_header_ = header;
    stream_source_cursor_ = 0;
    stream_source_rate_.store(header.sample_rate);
    stream_channels_.store(header.channels);
    stream_bits_.store(header.bits);
    stream_total_frames_.store(header.total_frames);
    stream_duration_ms_.store(static_cast<uint32_t>(
        (static_cast<uint64_t>(header.total_frames) * 1000U) / header.sample_rate));
    resetStreamBuffer(0, false);
    stream_state_.store(static_cast<uint8_t>(StreamState::Buffering));

    ESP_LOGI(TAG, "Streaming WAV: %s, %lu Hz, %uch/%ubit, %.1f sec",
             command.path,
             static_cast<unsigned long>(header.sample_rate),
             static_cast<unsigned>(header.channels),
             static_cast<unsigned>(header.bits),
             static_cast<double>(stream_duration_ms_.load()) / 1000.0);
}

void Audio::closeStreamFile() {
    if (stream_file_) {
        std::fclose(stream_file_);
        stream_file_ = nullptr;
    }
}

void Audio::resetStreamBuffer(uint32_t base_frame, bool preserve_pause) {
    stream_resetting_.store(true);
    stream_write_total_.store(0);
    stream_consumed_total_.store(0);
    stream_base_frame_.store(base_frame);
    stream_position_ms_.store(stream_source_rate_.load()
        ? static_cast<uint32_t>(
              (static_cast<uint64_t>(base_frame) * 1000U) / stream_source_rate_.load())
        : 0);
    stream_buffered_ms_.store(0);
    stream_eof_.store(false);
    stream_generation_.fetch_add(1);
    if (!preserve_pause) {
        stream_paused_.store(false);
    }
    stream_resetting_.store(false);
}

void Audio::fillStreamBuffer() {
    if (!stream_file_ || !stream_buffer_ || stream_resetting_.load()) {
        return;
    }

    const uint32_t write_total = stream_write_total_.load();
    const uint32_t consumed = stream_consumed_total_.load();
    const uint32_t used = write_total >= consumed ? write_total - consumed : 0;
    if (used + 2U >= config_.stream_buffer_frames) {
        updateStreamBufferingState();
        return;
    }

    const uint32_t free_frames = config_.stream_buffer_frames - used - 1U;
    const uint32_t total_frames = stream_header_.total_frames;
    if (stream_source_cursor_ >= total_frames) {
        stream_eof_.store(true);
        updateStreamBufferingState();
        return;
    }

    const uint32_t remaining = total_frames - stream_source_cursor_;
    const uint32_t requested_frames = std::min<uint32_t>(
        std::min<uint32_t>(free_frames, STREAM_READ_FRAMES), remaining);
    if (requested_frames == 0) {
        return;
    }

    static std::array<uint8_t, STREAM_READ_FRAMES * 4U> raw{};
    const size_t requested_bytes =
        static_cast<size_t>(requested_frames) * stream_header_.block_align;
    const size_t read_bytes = std::fread(raw.data(), 1, requested_bytes, stream_file_);
    const uint32_t frames_read =
        static_cast<uint32_t>(read_bytes / stream_header_.block_align);

    if (frames_read == 0) {
        if (std::ferror(stream_file_)) {
            stream_read_errors_.fetch_add(1);
            stream_state_.store(static_cast<uint8_t>(StreamState::Error));
        } else {
            stream_eof_.store(true);
        }
        if (std::ferror(stream_file_)) {
            closeStreamFile();
        }
        updateStreamBufferingState();
        return;
    }

    uint32_t new_write = write_total;
    for (uint32_t i = 0; i < frames_read; ++i) {
        const uint8_t* frame = raw.data() +
            static_cast<size_t>(i) * stream_header_.block_align;
        stream_buffer_[new_write % config_.stream_buffer_frames] = decodeFrame(frame);
        ++new_write;
    }
    stream_write_total_.store(new_write);
    stream_source_cursor_ += frames_read;

    if (frames_read < requested_frames || stream_source_cursor_ >= total_frames) {
        if (frames_read < requested_frames && std::ferror(stream_file_)) {
            stream_read_errors_.fetch_add(1);
        }
        stream_eof_.store(true);
    }

    updateStreamBufferingState();
}

void Audio::updateStreamBufferingState() {
    const StreamState state = streamState();
    if (state == StreamState::Stopped || state == StreamState::Error ||
        state == StreamState::Finished || state == StreamState::Opening) {
        return;
    }

    const uint32_t write_total = stream_write_total_.load();
    const uint32_t consumed = stream_consumed_total_.load();
    const uint32_t available = write_total > consumed ? write_total - consumed : 0;
    const uint32_t rate = stream_source_rate_.load();
    const uint32_t buffered_ms = rate
        ? static_cast<uint32_t>((static_cast<uint64_t>(available) * 1000U) / rate)
        : 0;
    stream_buffered_ms_.store(buffered_ms);

    if (stream_paused_.load()) {
        stream_state_.store(static_cast<uint8_t>(StreamState::Paused));
        return;
    }

    const uint32_t prefill_frames = rate
        ? static_cast<uint32_t>(
              (static_cast<uint64_t>(config_.stream_prefill_ms) * rate) / 1000U)
        : 1U;
    if (available >= std::max<uint32_t>(1U, prefill_frames) || stream_eof_.load()) {
        if (available > 0) {
            stream_state_.store(static_cast<uint8_t>(StreamState::Playing));
        } else if (stream_eof_.load()) {
            stream_state_.store(static_cast<uint8_t>(StreamState::Finished));
        }
    } else {
        stream_state_.store(static_cast<uint8_t>(StreamState::Buffering));
    }
}

esp_err_t Audio::parseWavHeader(FILE* file, WavHeader& header) const {
    if (!file) {
        return ESP_ERR_INVALID_ARG;
    }
    if (std::fseek(file, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    uint8_t riff[12]{};
    if (std::fread(riff, 1, sizeof(riff), file) != sizeof(riff) ||
        std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint16_t format = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint16_t block_align = 0;
    uint32_t sample_rate = 0;
    uint32_t data_size = 0;
    long data_offset = 0;
    bool have_format = false;

    for (uint32_t chunks = 0; chunks < 64U; ++chunks) {
        uint8_t chunk[8]{};
        if (std::fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) {
            break;
        }
        const uint32_t size = le32(chunk + 4);
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (size < 16U || size > 256U) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            std::array<uint8_t, 256> fmt{};
            if (std::fread(fmt.data(), 1, size, file) != size) {
                return ESP_ERR_INVALID_SIZE;
            }
            format = le16(fmt.data());
            channels = le16(fmt.data() + 2);
            sample_rate = le32(fmt.data() + 4);
            block_align = le16(fmt.data() + 12);
            bits = le16(fmt.data() + 14);
            have_format = true;
            if ((size & 1U) != 0U) {
                std::fseek(file, 1, SEEK_CUR);
            }
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data_offset = std::ftell(file);
            data_size = size;
            if (have_format) {
                break;
            }
            if (std::fseek(file, static_cast<long>(size + (size & 1U)), SEEK_CUR) != 0) {
                return ESP_FAIL;
            }
        } else {
            if (std::fseek(file, static_cast<long>(size + (size & 1U)), SEEK_CUR) != 0) {
                return ESP_FAIL;
            }
        }
    }

    if (!have_format || data_offset <= 0 || data_size == 0 || format != 1 ||
        (channels != 1 && channels != 2) || (bits != 8 && bits != 16) ||
        sample_rate < 8000 || sample_rate > 96000 || block_align == 0 ||
        block_align != channels * (bits / 8U) || block_align > 4) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint32_t frames = data_size / block_align;
    if (frames == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    header.data_offset = data_offset;
    header.data_size = data_size;
    header.sample_rate = sample_rate;
    header.total_frames = frames;
    header.block_align = block_align;
    header.channels = static_cast<uint8_t>(channels);
    header.bits = static_cast<uint8_t>(bits);
    return ESP_OK;
}

int16_t Audio::decodeFrame(const uint8_t* frame) const {
    if (stream_header_.bits == 8) {
        int32_t mixed = (static_cast<int32_t>(frame[0]) - 128) << 8;
        if (stream_header_.channels == 2) {
            const int32_t right = (static_cast<int32_t>(frame[1]) - 128) << 8;
            mixed = (mixed + right) / 2;
        }
        return static_cast<int16_t>(mixed);
    }

    const int16_t left = static_cast<int16_t>(le16(frame));
    if (stream_header_.channels == 1) {
        return left;
    }
    const int16_t right = static_cast<int16_t>(le16(frame + 2));
    return static_cast<int16_t>((static_cast<int32_t>(left) + right) / 2);
}

void Audio::initSineTable() {
    for (size_t i = 0; i < sine_table_.size(); ++i) {
        const double angle = TWO_PI * static_cast<double>(i) /
                             static_cast<double>(sine_table_.size());
        sine_table_[i] = static_cast<int16_t>(std::lround(std::sin(angle) * 32767.0));
    }
}

void Audio::cleanupI2s() {
    if (!tx_channel_) {
        return;
    }
    i2s_channel_disable(tx_channel_);
    i2s_del_channel(tx_channel_);
    tx_channel_ = nullptr;
}

void Audio::cleanupStreamResources() {
    stopRealtime();
    if (realtime_buffer_) {
        heap_caps_free(realtime_buffer_);
        realtime_buffer_ = nullptr;
    }
    realtime_capacity_frames_ = 0;

    if (stream_buffer_) {
        heap_caps_free(stream_buffer_);
        stream_buffer_ = nullptr;
    }
    stream_state_.store(static_cast<uint8_t>(StreamState::Stopped));
    stream_source_rate_.store(0);
    stream_total_frames_.store(0);
    stream_write_total_.store(0);
    stream_consumed_total_.store(0);
    stream_position_ms_.store(0);
    stream_duration_ms_.store(0);
    stream_buffered_ms_.store(0);
}

const char* effect_name(Effect effect) {
    switch (effect) {
        case Effect::Tick:
            return "TICK";
        case Effect::Click:
            return "CLICK";
        case Effect::Confirm:
            return "CONFIRM";
        case Effect::Back:
            return "BACK";
        case Effect::Error:
            return "ERROR";
        case Effect::Startup:
            return "STARTUP";
        case Effect::Coin:
            return "COIN";
        default:
            return "?";
    }
}

const char* waveform_name(Waveform waveform) {
    switch (waveform) {
        case Waveform::Sine:
            return "SINE";
        case Waveform::Square:
            return "SQUARE";
        case Waveform::Triangle:
            return "TRIANGLE";
        case Waveform::Noise:
            return "NOISE";
        case Waveform::Sawtooth:
            return "SAWTOOTH";
        default:
            return "?";
    }
}

const char* stream_state_name(StreamState state) {
    switch (state) {
        case StreamState::Stopped:
            return "STOPPED";
        case StreamState::Opening:
            return "OPENING";
        case StreamState::Buffering:
            return "BUFFERING";
        case StreamState::Playing:
            return "PLAYING";
        case StreamState::Paused:
            return "PAUSED";
        case StreamState::Finished:
            return "FINISHED";
        case StreamState::Error:
            return "ERROR";
        default:
            return "?";
    }
}

} // namespace pogopo::audio
