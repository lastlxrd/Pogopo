#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace pogopo::audio {

enum class Waveform : uint8_t {
    Sine,
    Square,
    Triangle,
    Noise,
};

enum class Effect : uint8_t {
    Tick,
    Click,
    Confirm,
    Back,
    Error,
    Startup,
    Coin,
};

struct Stats {
    uint32_t buffers_written = 0;
    uint32_t write_errors = 0;
    uint32_t short_writes = 0;
    uint32_t dropped_commands = 0;
    uint32_t last_write_us = 0;
    uint32_t max_write_us = 0;
    uint8_t active_voices = 0;
};

class Audio {
public:
    struct Config {
        int dout_io = 38;
        int bclk_io = 39;
        int lrck_io = 40;
        uint32_t sample_rate = 32768;
        uint8_t master_volume = 68;
        uint8_t queue_depth = 16;
        uint8_t dma_desc_num = 6;
        uint16_t dma_frame_num = 256;
        uint16_t render_frames = 256;
        uint32_t task_stack = 6144;
        UBaseType_t task_priority = 6;
        BaseType_t task_core = 0;
    };

    Audio() = default;
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    esp_err_t begin(const Config& config);
    void end();

    bool play(Effect effect);
    bool tone(uint16_t frequency_hz, uint16_t duration_ms,
              uint8_t volume = 70, Waveform waveform = Waveform::Sine);
    // Takes ownership of samples allocated with heap_caps_malloc()/malloc-compatible heap.
    // Mono signed 16-bit PCM; the audio task frees it after playback or StopAll.
    bool playPcmOwned(int16_t* samples, uint32_t frames, uint32_t sample_rate, uint8_t volume = 80);
    void stopAll();

    void setMasterVolume(uint8_t percent);
    uint8_t masterVolume() const { return master_volume_.load(); }

    bool ok() const { return ok_.load(); }
    bool active() const { return active_voices_.load() != 0 || pcm_active_.load(); }
    uint8_t activeVoices() const { return active_voices_.load(); }
    uint32_t sampleRate() const { return config_.sample_rate; }
    Stats stats() const;

private:
    static constexpr size_t MAX_VOICES = 4;
    static constexpr size_t SINE_TABLE_SIZE = 256;
    static constexpr size_t MAX_RENDER_FRAMES = 512;

    enum class CommandType : uint8_t {
        PlayEffect,
        PlayTone,
        PlayPcm,
        StopAll,
    };

    struct Command {
        CommandType type = CommandType::PlayEffect;
        Effect effect = Effect::Tick;
        Waveform waveform = Waveform::Sine;
        uint16_t frequency_hz = 0;
        uint16_t duration_ms = 0;
        uint8_t volume = 0;
        int16_t* pcm_samples = nullptr;
        uint32_t pcm_frames = 0;
        uint32_t pcm_sample_rate = 0;
    };

    struct Note {
        uint16_t frequency_hz;
        uint16_t duration_ms;
        uint8_t volume;
        Waveform waveform;
        uint8_t attack_ms;
        uint8_t release_ms;
    };

    struct PcmVoice {
        int16_t* samples = nullptr;
        uint32_t frames = 0;
        uint64_t position_q16 = 0;
        uint64_t step_q16 = 0;
        uint8_t volume = 80;
        bool active = false;
    };

    struct Voice {
        bool active = false;
        bool custom = false;
        Effect effect = Effect::Tick;
        const Note* notes = nullptr;
        size_t note_count = 0;
        size_t note_index = 0;
        Note custom_note{};
        uint32_t phase = 0;
        uint32_t phase_increment = 0;
        uint32_t samples_total = 0;
        uint32_t samples_left = 0;
        uint32_t attack_samples = 0;
        uint32_t release_samples = 0;
        uint32_t serial = 0;
    };

    static void task_entry(void* argument);
    void task_loop();
    void processCommands();
    bool enqueue(const Command& command);

    void startEffect(Effect effect);
    void startTone(const Command& command);
    void startPcm(Command& command);
    int32_t renderPcm();
    void clearPcm();
    Voice& chooseVoice(bool custom, Effect effect);
    void loadCurrentNote(Voice& voice);
    void advanceVoice(Voice& voice);
    int32_t renderVoice(Voice& voice);
    int16_t waveformSample(Voice& voice);
    uint16_t envelopeQ15(const Voice& voice) const;
    const Note* patternForEffect(Effect effect, size_t& count) const;
    void silenceVoices();
    void updateActiveVoiceCount();

    void initSineTable();
    void cleanupI2s();

    Config config_{};
    i2s_chan_handle_t tx_channel_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;

    std::array<int16_t, SINE_TABLE_SIZE> sine_table_{};
    std::array<Voice, MAX_VOICES> voices_{};
    uint32_t voice_serial_ = 0;
    uint32_t noise_state_ = 0xA5C31E27u;
    PcmVoice pcm_{};

    std::atomic<bool> ok_{false};
    std::atomic<uint8_t> master_volume_{68};
    std::atomic<uint8_t> active_voices_{0};
    std::atomic<bool> pcm_active_{false};
    std::atomic<uint32_t> buffers_written_{0};
    std::atomic<uint32_t> write_errors_{0};
    std::atomic<uint32_t> short_writes_{0};
    std::atomic<uint32_t> dropped_commands_{0};
    std::atomic<uint32_t> last_write_us_{0};
    std::atomic<uint32_t> max_write_us_{0};
};

const char* effect_name(Effect effect);
const char* waveform_name(Waveform waveform);

} // namespace pogopo::audio
