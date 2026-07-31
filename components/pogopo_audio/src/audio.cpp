#include "pogopo/audio/audio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

namespace pogopo::audio {
namespace {
constexpr char TAG[] = "pogopo_audio";
constexpr double TWO_PI = 6.28318530717958647692;
constexpr uint32_t WRITE_TIMEOUT_MS = 1000;
}

Audio::~Audio() {
    end();
}

esp_err_t Audio::begin(const Config& config) {
    if (ok_.load() || task_ || queue_ || tx_channel_) return ESP_ERR_INVALID_STATE;
    if (config.dout_io < 0 || config.bclk_io < 0 || config.lrck_io < 0 ||
        config.sample_rate < 8000 || config.queue_depth == 0 ||
        config.dma_desc_num < 2 || config.dma_frame_num == 0 ||
        config.render_frames == 0 || config.render_frames > MAX_RENDER_FRAMES) {
        return ESP_ERR_INVALID_ARG;
    }

    config_ = config;
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
    initSineTable();

    queue_ = xQueueCreate(config_.queue_depth, sizeof(Command));
    if (!queue_) return ESP_ERR_NO_MEM;

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = config_.dma_desc_num;
    channel_config.dma_frame_num = config_.dma_frame_num;
    channel_config.auto_clear_after_cb = true;
    channel_config.auto_clear_before_cb = false;

    esp_err_t err = i2s_new_channel(&channel_config, &tx_channel_, nullptr);
    if (err != ESP_OK) {
        vQueueDelete(queue_);
        queue_ = nullptr;
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
        cleanupI2s();
        vQueueDelete(queue_);
        queue_ = nullptr;
        return err;
    }

    // Fill the first DMA area with silence so startup never emits random samples.
    std::array<int16_t, 128 * 2> silence{};
    size_t bytes_loaded = 0;
    i2s_channel_preload_data(tx_channel_, silence.data(), sizeof(silence), &bytes_loaded);

    err = i2s_channel_enable(tx_channel_);
    if (err != ESP_OK) {
        cleanupI2s();
        vQueueDelete(queue_);
        queue_ = nullptr;
        return err;
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
        cleanupI2s();
        vQueueDelete(queue_);
        queue_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ok_.store(true);
    ESP_LOGI(TAG,
             "Audio ready: %lu Hz, 16-bit stereo Philips, DOUT=%d BCLK=%d LRCK=%d, DMA=%ux%u",
             static_cast<unsigned long>(config_.sample_rate),
             config_.dout_io, config_.bclk_io, config_.lrck_io,
             static_cast<unsigned>(config_.dma_desc_num),
             static_cast<unsigned>(config_.dma_frame_num));
    return ESP_OK;
}

void Audio::end() {
    ok_.store(false);
    if (task_) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (queue_) {
        Command pending;
        while (xQueueReceive(queue_, &pending, 0) == pdTRUE) {
            if (pending.type == CommandType::PlayPcm && pending.pcm_samples) heap_caps_free(pending.pcm_samples);
        }
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
    silenceVoices();
    clearPcm();
    active_voices_.store(0);
    cleanupI2s();
}

bool Audio::play(Effect effect) {
    Command command;
    command.type = CommandType::PlayEffect;
    command.effect = effect;
    return enqueue(command);
}

bool Audio::tone(uint16_t frequency_hz, uint16_t duration_ms,
                 uint8_t volume, Waveform waveform) {
    if (frequency_hz < 20 || duration_ms == 0) return false;
    const uint32_t sample_rate = config_.sample_rate ? config_.sample_rate : 32768;
    if (frequency_hz >= sample_rate / 2) return false;

    Command command;
    command.type = CommandType::PlayTone;
    command.waveform = waveform;
    command.frequency_hz = frequency_hz;
    command.duration_ms = duration_ms;
    command.volume = std::min<uint8_t>(volume, 100);
    return enqueue(command);
}

bool Audio::playPcmOwned(int16_t* samples, uint32_t frames, uint32_t sample_rate, uint8_t volume) {
    if (!samples || frames == 0 || sample_rate < 8000 || sample_rate > 96000) {
        if (samples) heap_caps_free(samples);
        return false;
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

void Audio::stopAll() {
    Command command;
    command.type = CommandType::StopAll;
    enqueue(command);
}

void Audio::setMasterVolume(uint8_t percent) {
    master_volume_.store(std::min<uint8_t>(percent, 100));
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
    if (!ok_.load() || !queue_) return false;
    if (xQueueSend(queue_, &command, 0) != pdTRUE) {
        dropped_commands_.fetch_add(1);
        return false;
    }
    return true;
}

void Audio::task_entry(void* argument) {
    static_cast<Audio*>(argument)->task_loop();
}

void Audio::task_loop() {
    std::array<int16_t, MAX_RENDER_FRAMES * 2> output{};

    while (true) {
        processCommands();

        for (size_t frame = 0; frame < config_.render_frames; ++frame) {
            int32_t mix = 0;
            for (auto& voice : voices_) {
                mix += renderVoice(voice);
            }
            mix += renderPcm();
            mix = std::clamp<int32_t>(mix, -32767, 32767);
            const int16_t sample = static_cast<int16_t>(mix);
            output[frame * 2] = sample;
            output[frame * 2 + 1] = sample;
        }
        updateActiveVoiceCount();

        const size_t requested_bytes = static_cast<size_t>(config_.render_frames) * 2U * sizeof(int16_t);
        size_t written_bytes = 0;
        const int64_t start_us = esp_timer_get_time();
        const esp_err_t err = i2s_channel_write(
            tx_channel_, output.data(), requested_bytes, &written_bytes, WRITE_TIMEOUT_MS);
        const uint32_t elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - start_us);
        last_write_us_.store(elapsed_us);

        uint32_t observed_max = max_write_us_.load();
        while (elapsed_us > observed_max &&
               !max_write_us_.compare_exchange_weak(observed_max, elapsed_us)) {}

        if (err != ESP_OK) {
            write_errors_.fetch_add(1);
            ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(err));
        } else {
            buffers_written_.fetch_add(1);
            if (written_bytes != requested_bytes) short_writes_.fetch_add(1);
        }
    }
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
            case CommandType::StopAll:
                silenceVoices();
                clearPcm();
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
        if (voice.serial < oldest->serial) oldest = &voice;
    }
    *oldest = Voice{};
    oldest->serial = ++voice_serial_;
    return *oldest;
}

void Audio::startEffect(Effect effect) {
    size_t count = 0;
    const Note* notes = patternForEffect(effect, count);
    if (!notes || count == 0) return;

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
        3,
        8,
    };
    voice.notes = &voice.custom_note;
    voice.note_count = 1;
    voice.note_index = 0;
    voice.phase = 0;
    loadCurrentNote(voice);
}

void Audio::startPcm(Command& command) {
    clearPcm();
    pcm_.samples = command.pcm_samples;
    pcm_.frames = command.pcm_frames;
    pcm_.position_q16 = 0;
    pcm_.step_q16 = (static_cast<uint64_t>(command.pcm_sample_rate) << 16U) / config_.sample_rate;
    pcm_.step_q16 = std::max<uint64_t>(1U, pcm_.step_q16);
    pcm_.volume = command.volume;
    pcm_.active = true;
    pcm_active_.store(true);
    command.pcm_samples = nullptr;
}

int32_t Audio::renderPcm() {
    if (!pcm_.active || !pcm_.samples || pcm_.frames == 0) return 0;
    const uint32_t index = static_cast<uint32_t>(pcm_.position_q16 >> 16U);
    if (index >= pcm_.frames) {
        clearPcm();
        return 0;
    }
    const int32_t raw = pcm_.samples[index];
    const int32_t sample = static_cast<int32_t>(
        (static_cast<int64_t>(raw) * pcm_.volume * master_volume_.load()) / 10000LL);
    pcm_.position_q16 += pcm_.step_q16;
    if ((pcm_.position_q16 >> 16U) >= pcm_.frames) clearPcm();
    return sample;
}

void Audio::clearPcm() {
    if (pcm_.samples) heap_caps_free(pcm_.samples);
    pcm_ = {};
    pcm_active_.store(false);
}

void Audio::loadCurrentNote(Voice& voice) {
    if (!voice.active || !voice.notes || voice.note_index >= voice.note_count) {
        voice.active = false;
        voice.samples_left = 0;
        return;
    }

    const Note& note = voice.notes[voice.note_index];
    voice.samples_total = std::max<uint32_t>(
        1U, static_cast<uint32_t>((static_cast<uint64_t>(note.duration_ms) * config_.sample_rate) / 1000U));
    voice.samples_left = voice.samples_total;
    voice.attack_samples = static_cast<uint32_t>(
        (static_cast<uint64_t>(note.attack_ms) * config_.sample_rate) / 1000U);
    voice.release_samples = static_cast<uint32_t>(
        (static_cast<uint64_t>(note.release_ms) * config_.sample_rate) / 1000U);
    voice.attack_samples = std::min(voice.attack_samples, voice.samples_total);
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
    if (!voice.active || !voice.notes || voice.samples_left == 0) return 0;
    const Note& note = voice.notes[voice.note_index];

    const int32_t raw = note.frequency_hz == 0 ? 0 : waveformSample(voice);
    const uint16_t envelope = envelopeQ15(voice);
    const uint32_t master = master_volume_.load();
    const int64_t scaled = static_cast<int64_t>(raw) * note.volume * master * envelope;
    const int32_t sample = static_cast<int32_t>(scaled / (100LL * 100LL * 32767LL));

    if (voice.samples_left > 0) --voice.samples_left;
    if (voice.samples_left == 0) advanceVoice(voice);
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
    }

    voice.phase += voice.phase_increment;
    return static_cast<int16_t>(std::clamp<int32_t>(value, -32767, 32767));
}

uint16_t Audio::envelopeQ15(const Voice& voice) const {
    if (voice.samples_total == 0) return 0;
    const uint32_t elapsed = voice.samples_total - voice.samples_left;
    uint32_t envelope = 32767U;

    if (voice.attack_samples > 0 && elapsed < voice.attack_samples) {
        envelope = (elapsed * 32767U) / voice.attack_samples;
    }
    if (voice.release_samples > 0 && voice.samples_left < voice.release_samples) {
        const uint32_t release = (voice.samples_left * 32767U) / voice.release_samples;
        envelope = std::min(envelope, release);
    }
    return static_cast<uint16_t>(envelope);
}

const Audio::Note* Audio::patternForEffect(Effect effect, size_t& count) const {
    static constexpr Note tick[] = {
        {1900, 22, 32, Waveform::Square, 1, 5},
    };
    static constexpr Note click[] = {
        {1250, 34, 58, Waveform::Triangle, 2, 7},
    };
    static constexpr Note confirm[] = {
        {620, 38, 62, Waveform::Sine, 3, 7},
        {0,   16,  0, Waveform::Sine, 0, 0},
        {930, 82, 72, Waveform::Sine, 3, 12},
    };
    static constexpr Note back[] = {
        {650, 38, 58, Waveform::Triangle, 2, 7},
        {0,   12,  0, Waveform::Sine, 0, 0},
        {360, 72, 62, Waveform::Triangle, 2, 12},
    };
    static constexpr Note error[] = {
        {190, 92, 49, Waveform::Square, 3, 12},
        {0,   34,  0, Waveform::Sine, 0, 0},
        {145, 125, 52, Waveform::Square, 3, 18},
    };
    static constexpr Note startup[] = {
        {523, 72, 58, Waveform::Sine, 4, 10},
        {0,   14,  0, Waveform::Sine, 0, 0},
        {659, 72, 62, Waveform::Sine, 4, 10},
        {0,   14,  0, Waveform::Sine, 0, 0},
        {784, 135, 72, Waveform::Sine, 4, 20},
    };
    static constexpr Note coin[] = {
        {988, 36, 62, Waveform::Square, 2, 6},
        {0,   10,  0, Waveform::Sine, 0, 0},
        {1319, 92, 70, Waveform::Sine, 2, 14},
    };

    switch (effect) {
        case Effect::Tick:    count = std::size(tick); return tick;
        case Effect::Click:   count = std::size(click); return click;
        case Effect::Confirm: count = std::size(confirm); return confirm;
        case Effect::Back:    count = std::size(back); return back;
        case Effect::Error:   count = std::size(error); return error;
        case Effect::Startup: count = std::size(startup); return startup;
        case Effect::Coin:    count = std::size(coin); return coin;
    }
    count = 0;
    return nullptr;
}

void Audio::silenceVoices() {
    for (auto& voice : voices_) voice = Voice{};
    active_voices_.store(0);
}

void Audio::updateActiveVoiceCount() {
    uint8_t count = 0;
    for (const auto& voice : voices_) {
        if (voice.active) ++count;
    }
    if (pcm_.active) ++count;
    active_voices_.store(count);
}

void Audio::initSineTable() {
    for (size_t i = 0; i < sine_table_.size(); ++i) {
        const double angle = TWO_PI * static_cast<double>(i) /
                             static_cast<double>(sine_table_.size());
        sine_table_[i] = static_cast<int16_t>(std::lround(std::sin(angle) * 32767.0));
    }
}

void Audio::cleanupI2s() {
    if (!tx_channel_) return;
    i2s_channel_disable(tx_channel_);
    i2s_del_channel(tx_channel_);
    tx_channel_ = nullptr;
}

const char* effect_name(Effect effect) {
    switch (effect) {
        case Effect::Tick: return "TICK";
        case Effect::Click: return "CLICK";
        case Effect::Confirm: return "CONFIRM";
        case Effect::Back: return "BACK";
        case Effect::Error: return "ERROR";
        case Effect::Startup: return "STARTUP";
        case Effect::Coin: return "COIN";
        default: return "?";
    }
}

const char* waveform_name(Waveform waveform) {
    switch (waveform) {
        case Waveform::Sine: return "SINE";
        case Waveform::Square: return "SQUARE";
        case Waveform::Triangle: return "TRIANGLE";
        case Waveform::Noise: return "NOISE";
        default: return "?";
    }
}

} // namespace pogopo::audio
