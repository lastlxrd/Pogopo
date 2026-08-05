#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

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

enum class StreamState : uint8_t {
    Stopped,
    Opening,
    Buffering,
    Playing,
    Paused,
    Finished,
    Error,
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

struct RealtimeInfo {
    bool active = false;
    uint32_t buffered_frames = 0;
    uint32_t capacity_frames = 0;
    uint32_t source_rate = 0;
    uint32_t underruns = 0;
    uint32_t overruns = 0;
    uint8_t volume = 0;
};

struct StreamInfo {
    StreamState state = StreamState::Stopped;
    uint32_t position_ms = 0;
    uint32_t duration_ms = 0;
    uint32_t buffered_ms = 0;
    uint32_t sample_rate = 0;
    uint32_t underruns = 0;
    uint32_t read_errors = 0;
    uint32_t dropped_commands = 0;
    uint8_t channels = 0;
    uint8_t bits_per_sample = 0;
    uint8_t volume = 0;
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
        uint8_t dma_desc_num = 8;
        uint16_t dma_frame_num = 512;
        uint16_t render_frames = 512;
        uint32_t task_stack = 8192;
        UBaseType_t task_priority = 8;
        BaseType_t task_core = 0;

        uint32_t realtime_buffer_frames = 4096;

        uint32_t stream_buffer_frames = 32768;
        uint16_t stream_prefill_ms = 120;
        uint8_t stream_queue_depth = 8;
        uint32_t stream_task_stack = 8192;
        UBaseType_t stream_task_priority = 4;
        BaseType_t stream_task_core = 1;
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
    bool playMusicPcmOwned(int16_t* samples, uint32_t frames,
                           uint32_t sample_rate, uint8_t volume = 80,
                           bool loop = true);
    void stopMusicPcm();
    void stopAll();

    // Low-latency stereo PCM ring used by emulators. The producer may run on
    // another core. When active it gets an exclusive fast path in the I2S task;
    // at 32768 Hz no interpolation or UI/WAV mixing runs in the hot loop.
    esp_err_t startRealtimeStereo(uint32_t sample_rate = 32768, uint8_t volume = 72);
    void stopRealtime();
    size_t pushRealtimeStereo(const int16_t* interleaved_stereo, size_t frames);
    RealtimeInfo realtimeInfo() const;

    bool playStream(const char* path, uint8_t volume = 82);
    bool pauseStream();
    bool resumeStream();
    bool toggleStreamPause();
    bool stopStream();
    bool seekStreamMs(uint32_t position_ms);
    StreamInfo streamInfo() const;
    StreamState streamState() const;
    bool streamActive() const;
    bool streamPlaying() const { return streamState() == StreamState::Playing; }

    void setMasterVolume(uint8_t percent);
    uint8_t masterVolume() const { return master_volume_.load(); }
    void setEnabled(bool enabled);
    bool enabled() const { return enabled_.load(); }

    bool ok() const { return ok_.load(); }
    bool active() const {
        return active_voices_.load() != 0 || pcm_active_.load() ||
               music_pcm_active_.load() ||
               realtime_active_.load() || streamActive();
    }
    uint8_t activeVoices() const { return active_voices_.load(); }
    uint32_t sampleRate() const { return config_.sample_rate; }
    Stats stats() const;

private:
    static constexpr size_t MAX_VOICES = 4;
    static constexpr size_t SINE_TABLE_SIZE = 256;
    static constexpr size_t MAX_RENDER_FRAMES = 512;
    static constexpr size_t STREAM_PATH_SIZE = 192;
    static constexpr size_t STREAM_READ_FRAMES = 1024;

    enum class CommandType : uint8_t {
        PlayEffect,
        PlayTone,
        PlayPcm,
        PlayMusicPcm,
        StopMusicPcm,
        StopAll,
    };

    enum class StreamCommandType : uint8_t {
        Play,
        Pause,
        Resume,
        Stop,
        Seek,
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
        bool pcm_loop = false;
    };

    struct StreamCommand {
        StreamCommandType type = StreamCommandType::Stop;
        uint8_t volume = 82;
        uint32_t position_ms = 0;
        char path[STREAM_PATH_SIZE]{};
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
        bool loop = false;
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

    struct WavHeader {
        long data_offset = 0;
        uint32_t data_size = 0;
        uint32_t sample_rate = 0;
        uint32_t total_frames = 0;
        uint16_t block_align = 0;
        uint8_t channels = 0;
        uint8_t bits = 0;
    };

    static void task_entry(void* argument);
    static void stream_task_entry(void* argument);
    void task_loop();
    void stream_task_loop();
    void processCommands();
    bool enqueue(const Command& command);
    bool enqueueStream(const StreamCommand& command);

    void startEffect(Effect effect);
    void startTone(const Command& command);
    void startPcm(Command& command);
    int32_t renderPcm();
    void startMusicPcm(Command& command);
    int32_t renderMusicPcm();
    int32_t renderStream();
    void renderRealtime(int32_t& left, int32_t& right);
    void clearPcm();
    void clearMusicPcm();
    Voice& chooseVoice(bool custom, Effect effect);
    void loadCurrentNote(Voice& voice);
    void advanceVoice(Voice& voice);
    int32_t renderVoice(Voice& voice);
    int16_t waveformSample(Voice& voice);
    uint16_t envelopeQ15(const Voice& voice) const;
    const Note* patternForEffect(Effect effect, size_t& count) const;
    void silenceVoices();
    void updateActiveVoiceCount();

    void handleStreamCommand(const StreamCommand& command);
    void openStreamFile(const StreamCommand& command);
    void closeStreamFile();
    void resetStreamBuffer(uint32_t base_frame, bool preserve_pause);
    void fillStreamBuffer();
    void updateStreamBufferingState();
    esp_err_t parseWavHeader(FILE* file, WavHeader& header) const;
    int16_t decodeFrame(const uint8_t* frame) const;

    void initSineTable();
    void cleanupI2s();
    void cleanupStreamResources();

    Config config_{};
    i2s_chan_handle_t tx_channel_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    QueueHandle_t stream_queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t stream_task_ = nullptr;

    std::array<int16_t, SINE_TABLE_SIZE> sine_table_{};
    std::array<Voice, MAX_VOICES> voices_{};
    uint32_t voice_serial_ = 0;
    uint32_t noise_state_ = 0xA5C31E27u;
    PcmVoice pcm_{};
    PcmVoice music_pcm_{};

    int16_t* realtime_buffer_ = nullptr;
    uint32_t realtime_capacity_frames_ = 0;
    uint32_t realtime_fraction_q16_ = 0;
    uint32_t realtime_step_q16_ = 1U << 16U;
    int32_t realtime_last_left_ = 0;
    int32_t realtime_last_right_ = 0;
    uint16_t realtime_fade_samples_ = 0;
    uint16_t realtime_fade_in_samples_ = 0;
    bool realtime_underrun_latched_ = false;

    int16_t* stream_buffer_ = nullptr;
    FILE* stream_file_ = nullptr;
    WavHeader stream_header_{};
    uint32_t stream_source_cursor_ = 0;
    uint64_t stream_position_q16_ = 0;
    uint64_t stream_step_q16_ = 0;
    uint32_t stream_generation_seen_ = 0;
    bool stream_underrun_latched_ = false;

    std::atomic<bool> ok_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<uint8_t> master_volume_{68};
    std::atomic<uint8_t> active_voices_{0};
    std::atomic<bool> pcm_active_{false};
    std::atomic<bool> music_pcm_active_{false};
    std::atomic<uint32_t> buffers_written_{0};
    std::atomic<uint32_t> write_errors_{0};
    std::atomic<uint32_t> short_writes_{0};
    std::atomic<uint32_t> dropped_commands_{0};
    std::atomic<uint32_t> last_write_us_{0};
    std::atomic<uint32_t> max_write_us_{0};

    std::atomic<bool> realtime_active_{false};
    std::atomic<uint8_t> realtime_volume_{72};
    std::atomic<uint32_t> realtime_source_rate_{32768};
    std::atomic<uint32_t> realtime_write_total_{0};
    std::atomic<uint32_t> realtime_read_total_{0};
    std::atomic<uint32_t> realtime_underruns_{0};
    std::atomic<uint32_t> realtime_overruns_{0};

    std::atomic<bool> stream_exit_{false};
    std::atomic<bool> stream_resetting_{false};
    std::atomic<bool> stream_paused_{false};
    std::atomic<bool> stream_eof_{false};
    std::atomic<uint8_t> stream_state_{static_cast<uint8_t>(StreamState::Stopped)};
    std::atomic<uint8_t> stream_volume_{82};
    std::atomic<uint8_t> stream_channels_{0};
    std::atomic<uint8_t> stream_bits_{0};
    std::atomic<uint32_t> stream_source_rate_{0};
    std::atomic<uint32_t> stream_total_frames_{0};
    std::atomic<uint32_t> stream_base_frame_{0};
    std::atomic<uint32_t> stream_write_total_{0};
    std::atomic<uint32_t> stream_consumed_total_{0};
    std::atomic<uint32_t> stream_generation_{0};
    std::atomic<uint32_t> stream_position_ms_{0};
    std::atomic<uint32_t> stream_duration_ms_{0};
    std::atomic<uint32_t> stream_buffered_ms_{0};
    std::atomic<uint32_t> stream_underruns_{0};
    std::atomic<uint32_t> stream_read_errors_{0};
    std::atomic<uint32_t> stream_dropped_commands_{0};
};

const char* effect_name(Effect effect);
const char* waveform_name(Waveform waveform);
const char* stream_state_name(StreamState state);

} // namespace pogopo::audio
