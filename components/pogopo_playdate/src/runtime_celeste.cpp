#include "pogopo/playdate/runtime.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <limits>
#include <new>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "celeste_assets.h"
#include "embedded_source.h"
#include "pdx_package.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "zlib.h"

namespace pogopo::playdate {
namespace {

static_assert(sizeof(lua_Integer) == 4 && sizeof(lua_Number) == 4,
    "PogoDate requires the Lua component and all API clients to use LUA_32BITS");

constexpr char TAG[] = "pogodate";
constexpr uint32_t kMaximumFps = 50;
constexpr size_t kErrorCapacity = 384;
constexpr char kImageMetatable[] = "PogoDate.Image";
constexpr char kImageTableMetatable[] = "PogoDate.ImageTable";
constexpr char kFontMetatable[] = "PogoDate.Font";
constexpr char kSoundMetatable[] = "PogoDate.Sound";
constexpr char kSynthMetatable[] = "PogoDate.Synth";
constexpr char kFileMetatable[] = "PogoDate.File";
char kTimerRegistryKey;

enum Pixel : uint8_t {
    Clear = 0,
    White = 1,
    Black = 2,
    Pattern = 3,
    Xor = 4,
};

// Public Playdate LCDSolidColor values are part of the Lua ABI.  They are not
// the same as Pixel above, whose zero is reserved for transparent image data.
// Keep the boundary explicit so packages that use literal 0/1/2/3 values work
// exactly like packages that read gfx.kColorBlack/White/Clear/XOR.
enum PlaydateColor : lua_Integer {
    PdColorBlack = 0,
    PdColorWhite = 1,
    PdColorClear = 2,
    PdColorXor = 3,
};

enum DrawMode : int {
    Copy = 0,
    WhiteTransparent = 1,
    BlackTransparent = 2,
    FillWhite = 3,
    FillBlack = 4,
    DrawXor = 5,
    Nxor = 6,
    Inverted = 7,
};

enum Flip : int {
    Unflipped = 0,
    FlippedX = 1,
    FlippedY = 2,
    FlippedXY = 3,
};

struct Image {
    int width = 0;
    int height = 0;
    int stride = 0;
    uint8_t* pixels = nullptr;
    bool owns_pixels = false;
    const CelesteAsset* asset = nullptr;
    int frame = 0;
    bool inverted = false;
    Image* mask = nullptr;
    int8_t pooled_slot = -1;
    // Compiled Playdate images keep their canvas-sized coordinates even when
    // only a small rectangle contains pixels.  Retaining that rectangle lets
    // the renderer skip transparent margins without changing image anchors.
    bool content_bounds_valid = false;
    int16_t content_left = 0;
    int16_t content_top = 0;
    int16_t content_right = 0;
    int16_t content_bottom = 0;
    bool black_bounds_valid = false;
    int16_t black_left = 0;
    int16_t black_top = 0;
    int16_t black_right = 0;
    int16_t black_bottom = 0;
};

struct ImageTable {
    const CelesteAsset* asset = nullptr;
    uint8_t* data = nullptr;
    size_t data_size = 0;
    uint16_t frame_width = 0;
    uint16_t frame_height = 0;
    uint16_t frame_count = 0;
};

struct PdFont {
    bool pico = false;
    bool compiled = false;
    int scale = 1;
    int16_t tracking = 0;
    uint8_t glyph_width = 0;
    uint8_t glyph_height = 0;
    uint32_t data_size = 0;

    const uint8_t* data() const {
        return reinterpret_cast<const uint8_t*>(this + 1);
    }
};

struct Sound {
    audio::Effect effect = audio::Effect::Click;
    bool playing = false;
    bool music = false;
    bool loop = false;
    float volume = 1.0f;
    float rate = 1.0f;
    int16_t cache_index = -1;
    char path[160]{};
};

struct Synth {
    int waveform = 2; // Playdate kWaveSine
    float left_volume = 1.0f;
    float right_volume = 1.0f;
    float attack = 0.003f;
    float decay = 0.0f;
    float sustain = 1.0f;
    float release = 0.008f;
    float transpose = 0.0f;
    uint32_t voice_token = 0;
    uint32_t ends_at_ms = 0;
    bool playing = false;
    bool indefinite = false;
};

struct CachedSound {
    int16_t* samples = nullptr;
    uint32_t frames = 0;
    uint32_t sample_rate = 0;
    char path[384]{};
};

struct PdFile {
    FILE* handle = nullptr;
    bool writable = false;
};

struct ClipRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

// Parse JSON directly into Lua values.  Keeping the parser here avoids a
// second object tree and its peak-memory cost, which matters for LDtk worlds on
// an ESP32-S3.  It implements RFC 8259 values, including escaped Unicode and
// surrogate pairs, and rejects excessive nesting and trailing input.
class JsonToLua {
public:
    JsonToLua(lua_State* state, const char* bytes, size_t size)
        : state_(state), begin_(bytes), cursor_(bytes), end_(bytes + size) {}

    bool parse(std::string& error) {
        skipWhitespace();
        if (!parseValue(0U)) return failResult(error);
        skipWhitespace();
        if (cursor_ != end_) {
            setError("unexpected trailing JSON data");
            lua_pop(state_, 1);
            return failResult(error);
        }
        return true;
    }

private:
    lua_State* state_ = nullptr;
    const char* begin_ = nullptr;
    const char* cursor_ = nullptr;
    const char* end_ = nullptr;
    const char* message_ = nullptr;

    bool failResult(std::string& error) const {
        const size_t offset = static_cast<size_t>(cursor_ - begin_);
        char text[128]{};
        std::snprintf(text, sizeof(text), "%s at byte %u",
                      message_ ? message_ : "invalid JSON",
                      static_cast<unsigned>(offset));
        error = text;
        return false;
    }

    bool setError(const char* message) {
        if (!message_) message_ = message;
        return false;
    }

    void skipWhitespace() {
        while (cursor_ < end_ && (*cursor_ == ' ' || *cursor_ == '\t' ||
               *cursor_ == '\r' || *cursor_ == '\n')) ++cursor_;
    }

    bool take(char expected) {
        if (cursor_ >= end_ || *cursor_ != expected) return false;
        ++cursor_;
        return true;
    }

    static void appendUtf8(std::string& output, uint32_t codepoint) {
        if (codepoint <= 0x7fU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        }
    }

    static int hexDigit(char value) {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    bool readHex4(uint32_t& value) {
        if (end_ - cursor_ < 4) return setError("truncated Unicode escape");
        value = 0;
        for (int index = 0; index < 4; ++index) {
            const int digit = hexDigit(*cursor_++);
            if (digit < 0) return setError("invalid Unicode escape");
            value = (value << 4U) | static_cast<uint32_t>(digit);
        }
        return true;
    }

    bool parseString(std::string& output) {
        if (!take('"')) return setError("expected JSON string");
        output.clear();
        while (cursor_ < end_) {
            const unsigned char value = static_cast<unsigned char>(*cursor_++);
            if (value == '"') return true;
            if (value < 0x20U) return setError("control character in JSON string");
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (cursor_ >= end_) return setError("truncated JSON escape");
            const char escape = *cursor_++;
            switch (escape) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    uint32_t codepoint = 0;
                    if (!readHex4(codepoint)) return false;
                    if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                        if (end_ - cursor_ < 6 || cursor_[0] != '\\' ||
                            cursor_[1] != 'u') {
                            return setError("missing low Unicode surrogate");
                        }
                        cursor_ += 2;
                        uint32_t low = 0;
                        if (!readHex4(low) || low < 0xdc00U || low > 0xdfffU) {
                            return setError("invalid low Unicode surrogate");
                        }
                        codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) +
                                    (low - 0xdc00U);
                    } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                        return setError("unexpected low Unicode surrogate");
                    }
                    appendUtf8(output, codepoint);
                    break;
                }
                default: return setError("invalid JSON escape");
            }
        }
        return setError("unterminated JSON string");
    }

    bool parseLiteral(const char* literal, int type) {
        const size_t length = std::strlen(literal);
        if (static_cast<size_t>(end_ - cursor_) < length ||
            std::memcmp(cursor_, literal, length) != 0) {
            return setError("invalid JSON literal");
        }
        cursor_ += length;
        if (type < 0) lua_pushnil(state_);
        else lua_pushboolean(state_, type);
        return true;
    }

    bool parseNumber() {
        const char* start = cursor_;
        if (cursor_ < end_ && *cursor_ == '-') ++cursor_;
        if (cursor_ >= end_) return setError("truncated JSON number");
        if (*cursor_ == '0') {
            ++cursor_;
        } else {
            if (*cursor_ < '1' || *cursor_ > '9') return setError("invalid JSON number");
            while (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9') ++cursor_;
        }
        bool integral = true;
        if (cursor_ < end_ && *cursor_ == '.') {
            integral = false;
            ++cursor_;
            if (cursor_ >= end_ || *cursor_ < '0' || *cursor_ > '9') {
                return setError("invalid JSON fraction");
            }
            while (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9') ++cursor_;
        }
        if (cursor_ < end_ && (*cursor_ == 'e' || *cursor_ == 'E')) {
            integral = false;
            ++cursor_;
            if (cursor_ < end_ && (*cursor_ == '+' || *cursor_ == '-')) ++cursor_;
            if (cursor_ >= end_ || *cursor_ < '0' || *cursor_ > '9') {
                return setError("invalid JSON exponent");
            }
            while (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9') ++cursor_;
        }
        const std::string text(start, static_cast<size_t>(cursor_ - start));
        if (integral) {
            char* parsed_end = nullptr;
            const long long value = std::strtoll(text.c_str(), &parsed_end, 10);
            if (parsed_end && *parsed_end == '\0' &&
                value >= std::numeric_limits<lua_Integer>::min() &&
                value <= std::numeric_limits<lua_Integer>::max()) {
                lua_pushinteger(state_, static_cast<lua_Integer>(value));
                return true;
            }
        }
        char* parsed_end = nullptr;
        const float value = std::strtof(text.c_str(), &parsed_end);
        if (!parsed_end || *parsed_end != '\0' || !std::isfinite(value)) {
            return setError("JSON number is outside the supported range");
        }
        lua_pushnumber(state_, static_cast<lua_Number>(value));
        return true;
    }

    bool parseArray(unsigned depth) {
        if (!take('[')) return false;
        lua_createtable(state_, 0, 0);
        const int table = lua_absindex(state_, -1);
        skipWhitespace();
        if (take(']')) return true;
        lua_Integer index = 1;
        for (;;) {
            skipWhitespace();
            if (!parseValue(depth + 1U)) {
                lua_pop(state_, 1);
                return false;
            }
            lua_rawseti(state_, table, index++);
            skipWhitespace();
            if (take(']')) return true;
            if (!take(',')) {
                lua_pop(state_, 1);
                return setError("expected ',' or ']' in JSON array");
            }
        }
    }

    bool parseObject(unsigned depth) {
        if (!take('{')) return false;
        lua_createtable(state_, 0, 0);
        const int table = lua_absindex(state_, -1);
        skipWhitespace();
        if (take('}')) return true;
        std::string key;
        for (;;) {
            skipWhitespace();
            if (!parseString(key)) {
                lua_pop(state_, 1);
                return false;
            }
            skipWhitespace();
            if (!take(':')) {
                lua_pop(state_, 1);
                return setError("expected ':' in JSON object");
            }
            skipWhitespace();
            lua_pushlstring(state_, key.data(), key.size());
            if (!parseValue(depth + 1U)) {
                lua_pop(state_, 2);
                return false;
            }
            lua_settable(state_, table);
            skipWhitespace();
            if (take('}')) return true;
            if (!take(',')) {
                lua_pop(state_, 1);
                return setError("expected ',' or '}' in JSON object");
            }
        }
    }

    bool parseValue(unsigned depth) {
        if (depth > 64U) return setError("JSON nesting is too deep");
        if (!lua_checkstack(state_, 4)) return setError("Lua stack exhausted by JSON");
        skipWhitespace();
        if (cursor_ >= end_) return setError("unexpected end of JSON");
        if (*cursor_ == '{') return parseObject(depth);
        if (*cursor_ == '[') return parseArray(depth);
        if (*cursor_ == '"') {
            std::string text;
            if (!parseString(text)) return false;
            lua_pushlstring(state_, text.data(), text.size());
            return true;
        }
        if (*cursor_ == 't') return parseLiteral("true", 1);
        if (*cursor_ == 'f') return parseLiteral("false", 0);
        if (*cursor_ == 'n') return parseLiteral("null", -1);
        if (*cursor_ == '-' || (*cursor_ >= '0' && *cursor_ <= '9')) {
            return parseNumber();
        }
        return setError("unexpected JSON token");
    }
};

std::string adaptPlaydateLua(const char* source, size_t size) {
    std::string output(source, size);
    size_t position = 0;
    while (position + 1 < output.size()) {
        const char operation = output[position];
        if ((operation != '+' && operation != '-' && operation != '*' &&
             operation != '/') || output[position + 1] != '=') {
            ++position;
            continue;
        }

        size_t end = position;
        while (end > 0 && (output[end - 1] == ' ' || output[end - 1] == '\t')) {
            --end;
        }
        size_t start = end;
        while (start > 0) {
            const char value = output[start - 1];
            const bool lhs = (value >= 'a' && value <= 'z') ||
                             (value >= 'A' && value <= 'Z') ||
                             (value >= '0' && value <= '9') || value == '_' ||
                             value == '.' || value == ']' || value == '[';
            if (!lhs) break;
            --start;
        }
        if (start == end) {
            position += 2;
            continue;
        }
        const std::string lhs = output.substr(start, end - start);
        const std::string replacement = lhs + " = " + lhs + " " + operation + " ";
        output.replace(start, position + 2 - start, replacement);
        position = start + replacement.size();
    }
    return output;
}

audio::Effect effectForPath(const char* path) {
    if (!path) return audio::Effect::Click;
    const char* sfx = std::strstr(path, "sfx");
    const int index = sfx ? std::atoi(sfx + 3) : -1;
    if (index == 3 || index == 35 || index == 37 || index == 54) {
        return audio::Effect::Coin;
    }
    if (index == 0 || index == 16 || index == 23) {
        return audio::Effect::Error;
    }
    if (index == 5 || index == 7 || index == 55) {
        return audio::Effect::Confirm;
    }
    return audio::Effect::Click;
}

} // namespace

struct Runtime::Impl {
    lua_State* lua = nullptr;
    gfx::Canvas* canvas = nullptr;
    audio::Audio* audio = nullptr;
    storage::Storage* storage = nullptr;
    Game game = Game::PDSnake;
    PackageInfo package_info{};
    PdzArchive pdz{};
    PdzArchive external_pdz{};
    bool package_mode = false;

    bool is_running = false;
    bool inverted_display = false;
    uint8_t draw_color = Black;
    uint8_t solid_draw_color = Black;
    uint8_t draw_pattern_color = Black;
    bool draw_pattern_transparent = false;
    std::array<uint8_t, 8> draw_pattern{{0xff, 0xff, 0xff, 0xff,
                                         0xff, 0xff, 0xff, 0xff}};
    std::array<uint8_t, 64> draw_pattern_pixels{};
    bool draw_pattern_uses_image = false;
    int pattern_offset_x = 0;
    int pattern_offset_y = 0;
    uint8_t background_color = White;
    int draw_mode = Copy;
    int line_width = 1;
    int stroke_location = 0;
    int display_scale = 1;
    int display_offset_x = 0;
    int display_offset_y = 0;
    int draw_offset_x = 0;
    int draw_offset_y = 0;
    uint32_t refresh_rate = 50;
    uint32_t frame_accumulator_units = 0;
    uint32_t frame_dt_ms = 20;
    uint32_t now_ms = 0;
    uint32_t elapsed_reset_ms = 0;
    uint8_t held_buttons = 0;
    uint8_t pressed_buttons = 0;
    uint8_t previous_held_buttons = 0;
    float crank_position = 0.0f;
    double crank_unwrapped_position = 0.0;
    double crank_tick_previous_position = 0.0;
    float crank_pending_change = 0.0f;
    float crank_pending_accelerated_change = 0.0f;
    uint32_t crank_sample_ms = 0;
    bool crank_initialized = false;
    bool crank_docked = false;
    int8_t crank_dock_event = 0;
    bool crank_sounds_disabled = false;
    float accelerometer_x = 0.0f;
    float accelerometer_y = 0.0f;
    float accelerometer_z = 1.0f;
    bool accelerometer_valid = false;
    bool accelerometer_running = false;
    uint32_t next_timer_id = 1;

    static constexpr size_t kMaximumCachedSounds = 32;
    static constexpr size_t kMaximumSoundCacheBytes = 2U * 1024U * 1024U;
    static constexpr size_t kMaximumCachedSoundBytes = 512U * 1024U;
    std::array<CachedSound, kMaximumCachedSounds> sound_cache{};
    size_t sound_cache_bytes = 0;

    Image screen{};
    bool screen_in_internal_ram = false;
    Image* target = nullptr;
    Image* stencil = nullptr;
    PdFont* current_font = nullptr;
    int current_font_ref = LUA_NOREF;
    int system_font_ref = LUA_NOREF;
    int maze_completion_image_ref = LUA_NOREF;
    bool maze_completion_reuse_logged = false;
    ClipRect clip{};
    struct Context {
        Image* target = nullptr;
        ClipRect clip{};
        Image* stencil = nullptr;
    };
    std::array<Context, 8> context_stack{};
    size_t context_depth = 0;

    // Full-screen scratch images are common in Playdate transitions.  Putting
    // their 96 KiB pixel planes inside Lua userdata makes every animation
    // frame grow the Lua heap and eventually forces a long full collection.
    // Reuse a small PSRAM-backed plane pool while keeping only the lightweight
    // Image userdata under Lua's ownership.
    struct LargeImageSlot {
        uint8_t* pixels = nullptr;
        size_t capacity = 0;
        bool in_use = false;
    };
    // Maze's LevelComplete animation creates a fresh 324x137 (44,388-byte)
    // render target every frame. The previous 64 KiB cutoff missed it, so the
    // buffer lived inside Lua userdata and repeatedly forced long collections
    // on ESP32-S3. Pool render targets from 32 KiB upward in PSRAM instead.
    static constexpr size_t kLargeImageThreshold = 32U * 1024U;
    std::array<LargeImageSlot, 6> large_image_pool{};

    size_t allocated_bytes = 0;
    size_t peak_allocated_bytes = 0;
    Stats runtime_stats{};
    std::array<bool, PdzArchive::MAX_ENTRIES> loaded_modules{};
    std::array<std::array<char, 128>, 32> loaded_external_modules{};
    size_t loaded_external_count = 0;
    unsigned external_import_depth = 0;
    char last_error[kErrorCapacity]{};

    static Impl* self(lua_State* state) {
        return static_cast<Impl*>(lua_touserdata(state, lua_upvalueindex(1)));
    }

    static void* allocator(void* user, void* pointer, size_t old_size,
                           size_t new_size) {
        auto* runtime = static_cast<Impl*>(user);
        if (!pointer) old_size = 0;
        if (new_size == 0) {
            if (pointer) {
                heap_caps_free(pointer);
                runtime->allocated_bytes = old_size <= runtime->allocated_bytes
                    ? runtime->allocated_bytes - old_size : 0;
            }
            return nullptr;
        }
        void* result = heap_caps_realloc(pointer, new_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!result) result = heap_caps_realloc(pointer, new_size, MALLOC_CAP_8BIT);
        if (!result) return nullptr;
        if (new_size >= old_size) runtime->allocated_bytes += new_size - old_size;
        else runtime->allocated_bytes -= std::min(runtime->allocated_bytes,
                                                   old_size - new_size);
        runtime->peak_allocated_bytes = std::max(runtime->peak_allocated_bytes,
                                                  runtime->allocated_bytes);
        return result;
    }

    void pushFunction(lua_CFunction function) {
        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, function, 1);
    }

    void setFunction(int table_index, const char* name, lua_CFunction function) {
        table_index = lua_absindex(lua, table_index);
        pushFunction(function);
        lua_setfield(lua, table_index, name);
    }

    void setError(const char* context, const char* message) {
        std::snprintf(last_error, sizeof(last_error), "%s: %s",
                      context ? context : "Lua", message ? message : "unknown");
        ++runtime_stats.errors;
        ESP_LOGE(TAG, "%s", last_error);
    }

    bool takeLuaError(const char* context) {
        const char* message = lua_tostring(lua, -1);
        setError(context, message);
        lua_pop(lua, 1);
        return false;
    }

    bool loadBuffer(const char* name, const char* source, size_t size) {
        const std::string adapted = adaptPlaydateLua(source, size);
        if (luaL_loadbuffer(lua, adapted.data(), adapted.size(), name) != LUA_OK) {
            return takeLuaError(name);
        }
        if (lua_pcall(lua, 0, 0, 0) != LUA_OK) return takeLuaError(name);
        return true;
    }

    bool loadBytecode(const char* name, const uint8_t* bytes, size_t size) {
        if (!bytes || size < 4U || bytes[0] != 0x1BU ||
            std::memcmp(bytes + 1U, "Lua", 3U) != 0) {
            setError(name, "PDZ record is not Lua bytecode");
            return false;
        }
        if (luaL_loadbufferx(lua, reinterpret_cast<const char*>(bytes), size,
                             name, "b") != LUA_OK) {
            return takeLuaError(name);
        }
        if (lua_pcall(lua, 0, 0, 0) != LUA_OK) return takeLuaError(name);
        return true;
    }

    const EmbeddedSource* sources(size_t& count) const {
        if (game == Game::Celeste) return celesteSources(count);
        if (game == Game::PDSnake) return pdsnakeSources(count);
        count = 0;
        return nullptr;
    }

    const EmbeddedSource* findSource(const char* requested, size_t& index) const {
        if (!requested || !requested[0]) return nullptr;
        char normalized[128]{};
        const size_t length = std::strlen(requested);
        if (length >= sizeof(normalized) - 5U) return nullptr;
        std::memcpy(normalized, requested, length);
        if (length < 4U || std::strcmp(requested + length - 4U, ".lua") != 0) {
            std::memcpy(normalized + length, ".lua", 5U);
        }
        size_t count = 0;
        const EmbeddedSource* list = sources(count);
        for (size_t i = 0; i < count; ++i) {
            if (std::strcmp(list[i].name, normalized) == 0) {
                index = i;
                return &list[i];
            }
        }
        return nullptr;
    }

    bool importModule(const char* requested) {
        if (!requested) return false;
        // Most CoreLibs are supplied by compat.lua because their stock
        // versions expect lower-level SDK objects we do not expose.  Pure-Lua
        // helpers should instead execute the exact copy bundled in the PDX.
        // Pretending these imports succeeded without executing them left
        // gfx.nineSlice nil in Maze even though its main.pdz contains the
        // official implementation.
        if (std::strncmp(requested, "CoreLibs/", 9) == 0) {
            const bool bundled_pure_lua =
                std::strcmp(requested, "CoreLibs/easing") == 0 ||
                std::strcmp(requested, "CoreLibs/animation") == 0 ||
                std::strcmp(requested, "CoreLibs/animator") == 0 ||
                std::strcmp(requested, "CoreLibs/nineslice") == 0 ||
                std::strcmp(requested, "CoreLibs/ui") == 0 ||
                std::strcmp(requested, "CoreLibs/ui/gridview") == 0 ||
                std::strcmp(requested, "CoreLibs/qrcode") == 0 ||
                std::strcmp(requested,
                            "CoreLibs/3rdparty/qrencode_panic_mod") == 0;
            if (!bundled_pure_lua || !package_mode || !pdz.findLua(requested)) {
                return true;
            }
        }
        if (package_mode) {
            PdzArchive* archive = &pdz;
            const PdzEntry* entry = archive->findLua(requested);
            bool external = false;
            bool opened_external = false;
            if (!entry) {
                if (external_pdz.path()[0]) {
                    entry = external_pdz.findLua(requested);
                    if (entry) {
                        archive = &external_pdz;
                        external = true;
                    }
                }
                if (!entry && external_import_depth == 0U) {
                    char module[128]{};
                    std::snprintf(module, sizeof(module), "%s", requested);
                    size_t length = std::strlen(module);
                    if (length > 4U &&
                        std::strcmp(module + length - 4U, ".lua") == 0) {
                        module[length - 4U] = '\0';
                    }
                    char archive_path[384]{};
                    char archive_error[128]{};
                    if (module[0] && !std::strstr(module, "..") &&
                        module[0] != '/' && pdxJoinPath(
                            archive_path, sizeof(archive_path), package_info.path,
                            module, ".pdz") &&
                        external_pdz.open(archive_path, archive_error,
                                          sizeof(archive_error), false) == ESP_OK) {
                        entry = external_pdz.findLua(requested);
                        if (entry) {
                            archive = &external_pdz;
                            external = true;
                            opened_external = true;
                        } else {
                            external_pdz.close();
                        }
                    }
                }
                if (!entry) {
                    setError("import", requested);
                    return false;
                }
            }
            const size_t slot = entry->slot;
            if (!external && slot >= loaded_modules.size()) {
                setError("import", "PDZ module index overflow");
                return false;
            }
            if (!external && loaded_modules[slot]) return true;
            if (external) {
                for (size_t index = 0; index < loaded_external_count; ++index) {
                    if (!std::strcmp(loaded_external_modules[index].data(),
                                     entry->name)) {
                        if (opened_external) external_pdz.close();
                        return true;
                    }
                }
                if (loaded_external_count >= loaded_external_modules.size()) {
                    if (opened_external) external_pdz.close();
                    setError("import", "too many external PDZ modules");
                    return false;
                }
            }
            uint8_t* bytecode = nullptr;
            size_t bytecode_size = 0;
            char archive_error[128]{};
            const esp_err_t err = archive->load(*entry, bytecode, bytecode_size,
                                                archive_error, sizeof(archive_error));
            if (err != ESP_OK) {
                setError(entry->name, archive_error);
                if (opened_external) external_pdz.close();
                return false;
            }
            if (!normalizePlaydateLuaBytecode(bytecode, bytecode_size,
                                              archive_error,
                                              sizeof(archive_error))) {
                setError(entry->name, archive_error);
                heap_caps_free(bytecode);
                if (opened_external) external_pdz.close();
                return false;
            }
            if (external) {
                std::snprintf(loaded_external_modules[loaded_external_count].data(),
                              loaded_external_modules[loaded_external_count].size(),
                              "%s", entry->name);
                ++loaded_external_count;
                ++external_import_depth;
            } else {
                loaded_modules[slot] = true;
            }
            const int64_t started = esp_timer_get_time();
            ESP_LOGI(TAG, "PDZ exec begin: %s (%u bytes)", entry->name,
                     static_cast<unsigned>(bytecode_size));
            const bool ok = loadBytecode(entry->name, bytecode, bytecode_size);
            const uint32_t elapsed_ms = static_cast<uint32_t>(
                std::max<int64_t>(0, esp_timer_get_time() - started) / 1000);
            ESP_LOGI(TAG, "PDZ exec %s: %s in %lu ms", ok ? "ready" : "failed",
                     entry->name, static_cast<unsigned long>(elapsed_ms));
            heap_caps_free(bytecode);
            if (external) {
                --external_import_depth;
                if (!ok && loaded_external_count > 0U) {
                    loaded_external_modules[--loaded_external_count].fill('\0');
                }
                if (opened_external) external_pdz.close();
            } else if (!ok) {
                loaded_modules[slot] = false;
            }
            return ok;
        }
        size_t index = 0;
        const EmbeddedSource* source = findSource(requested, index);
        if (!source || index >= loaded_modules.size()) {
            setError("import", requested);
            return false;
        }
        if (loaded_modules[index]) return true;
        loaded_modules[index] = true;
        if (!loadBuffer(source->name, source->data, source->size)) {
            loaded_modules[index] = false;
            return false;
        }
        return true;
    }

    static int cImport(lua_State* state) {
        Impl* runtime = self(state);
        const char* name = luaL_checkstring(state, 1);
        if (!runtime->importModule(name)) {
            return luaL_error(state, "%s", runtime->last_error);
        }
        return 0;
    }

    bool allocateImage(Image& image, int width, int height, uint8_t color,
                       bool prefer_internal = false,
                       bool* allocated_internal = nullptr) {
        if (width <= 0 || height <= 0 || width > 1024 || height > 1024 ||
            static_cast<size_t>(width) * height > 1024U * 1024U) return false;
        const size_t bytes = static_cast<size_t>(width) * height;
        uint8_t* pixels = nullptr;
        if (allocated_internal) *allocated_internal = false;
        if (prefer_internal) {
            pixels = static_cast<uint8_t*>(heap_caps_realloc(
                nullptr, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (pixels && allocated_internal) *allocated_internal = true;
        }
        if (!pixels) pixels = static_cast<uint8_t*>(heap_caps_realloc(
            nullptr, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!pixels && !prefer_internal) pixels = static_cast<uint8_t*>(
            heap_caps_realloc(nullptr, bytes, MALLOC_CAP_8BIT));
        if (!pixels) return false;
        std::memset(pixels, color, bytes);
        image.width = width;
        image.height = height;
        image.stride = width;
        image.pixels = pixels;
        image.owns_pixels = true;
        image.asset = nullptr;
        image.frame = 0;
        setSolidImageBounds(image, color);
        return true;
    }

    bool resizeScreen(int scale) {
        const int width = 400 / std::max(1, scale);
        const int height = 240 / std::max(1, scale);
        if (screen.pixels && screen.width == width && screen.height == height) {
            return true;
        }
        Image replacement{};
        const size_t bytes = static_cast<size_t>(width) * height;
        // A native 400x240 frame is read and written several times per game
        // update. Keep it in fast internal SRAM when a 64 KiB safety reserve
        // remains; otherwise fall back to PSRAM exactly as before.
        const size_t largest_internal = heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const bool prefer_internal = scale == 2 ||
            largest_internal >= bytes + 64U * 1024U;
        bool replacement_internal = false;
        if (!allocateImage(replacement, width, height, background_color,
                           prefer_internal, &replacement_internal)) {
            return false;
        }
        releaseImage(screen);
        screen = replacement;
        screen_in_internal_ram = replacement_internal;
        return true;
    }

    static void releaseImage(Image& image) {
        if (image.owns_pixels && image.pixels) heap_caps_free(image.pixels);
        image = {};
    }

    void clearLargeImagePool() {
        for (LargeImageSlot& slot : large_image_pool) {
            if (slot.pixels) heap_caps_free(slot.pixels);
            slot = {};
        }
    }

    Image* pushImage() {
        auto* image = static_cast<Image*>(lua_newuserdatauv(lua, sizeof(Image), 1));
        new (image) Image{};
        luaL_getmetatable(lua, kImageMetatable);
        lua_setmetatable(lua, -2);
        return image;
    }

    static void setContentBounds(Image& image, int right, int bottom,
                                 int left = 0, int top = 0) {
        image.content_bounds_valid = true;
        image.content_left = static_cast<int16_t>(
            std::clamp(left, 0, image.width));
        image.content_top = static_cast<int16_t>(
            std::clamp(top, 0, image.height));
        image.content_right = static_cast<int16_t>(
            std::clamp(right, static_cast<int>(image.content_left),
                       image.width));
        image.content_bottom = static_cast<int16_t>(
            std::clamp(bottom, static_cast<int>(image.content_top),
                       image.height));
    }

    static void expandContentBounds(Image& image, int left, int top,
                                    int right, int bottom) {
        left = std::clamp(left, 0, image.width);
        top = std::clamp(top, 0, image.height);
        right = std::clamp(right, left, image.width);
        bottom = std::clamp(bottom, top, image.height);
        if (left >= right || top >= bottom) return;
        if (!image.content_bounds_valid ||
            image.content_left >= image.content_right ||
            image.content_top >= image.content_bottom) {
            setContentBounds(image, right, bottom, left, top);
            return;
        }
        image.content_left = static_cast<int16_t>(
            std::min<int>(image.content_left, left));
        image.content_top = static_cast<int16_t>(
            std::min<int>(image.content_top, top));
        image.content_right = static_cast<int16_t>(
            std::max<int>(image.content_right, right));
        image.content_bottom = static_cast<int16_t>(
            std::max<int>(image.content_bottom, bottom));
    }

    static void setBlackBounds(Image& image, int right, int bottom,
                               int left = 0, int top = 0) {
        image.black_bounds_valid = true;
        image.black_left = static_cast<int16_t>(
            std::clamp(left, 0, image.width));
        image.black_top = static_cast<int16_t>(
            std::clamp(top, 0, image.height));
        image.black_right = static_cast<int16_t>(
            std::clamp(right, static_cast<int>(image.black_left),
                       image.width));
        image.black_bottom = static_cast<int16_t>(
            std::clamp(bottom, static_cast<int>(image.black_top),
                       image.height));
    }

    static void expandBlackBounds(Image& image, int left, int top,
                                  int right, int bottom) {
        left = std::clamp(left, 0, image.width);
        top = std::clamp(top, 0, image.height);
        right = std::clamp(right, left, image.width);
        bottom = std::clamp(bottom, top, image.height);
        if (left >= right || top >= bottom) return;
        if (!image.black_bounds_valid || image.black_left >= image.black_right ||
            image.black_top >= image.black_bottom) {
            setBlackBounds(image, right, bottom, left, top);
            return;
        }
        image.black_left = static_cast<int16_t>(
            std::min<int>(image.black_left, left));
        image.black_top = static_cast<int16_t>(
            std::min<int>(image.black_top, top));
        image.black_right = static_cast<int16_t>(
            std::max<int>(image.black_right, right));
        image.black_bottom = static_cast<int16_t>(
            std::max<int>(image.black_bottom, bottom));
    }

    static void setSolidImageBounds(Image& image, uint8_t color) {
        setContentBounds(image, color == Clear ? 0 : image.width,
                         color == Clear ? 0 : image.height);
        if (color == Black) {
            setBlackBounds(image, image.width, image.height);
        } else if (color == White || color == Clear) {
            setBlackBounds(image, 0, 0);
        } else {
            image.black_bounds_valid = false;
        }
    }

    static void logicalContentBounds(const Image& image, int flip,
                                     int& left, int& top,
                                     int& right, int& bottom,
                                     bool black_only = false) {
        if (black_only && image.black_bounds_valid) {
            left = image.black_left;
            top = image.black_top;
            right = image.black_right;
            bottom = image.black_bottom;
        } else if (image.content_bounds_valid) {
            left = image.content_left;
            top = image.content_top;
            right = image.content_right;
            bottom = image.content_bottom;
        } else {
            left = top = 0;
            right = image.width;
            bottom = image.height;
        }
        if (flip & FlippedX) {
            const int old_left = left;
            left = image.width - right;
            right = image.width - old_left;
        }
        if (flip & FlippedY) {
            const int old_top = top;
            top = image.height - bottom;
            bottom = image.height - old_top;
        }
    }

    Image* pushDynamicImage(int width, int height, uint8_t color) {
        if (width <= 0 || height <= 0 || width > 1024 || height > 1024 ||
            static_cast<size_t>(width) * height > 1024U * 1024U) {
            lua_pushnil(lua);
            return nullptr;
        }
        const size_t bytes = static_cast<size_t>(width) * height;
        if (bytes >= kLargeImageThreshold) {
            // Advance the collector so the previous frame's image userdata
            // promptly returns its plane to the pool.  This bounded step is
            // much cheaper than allowing several megabytes of dead 400x240
            // images to accumulate and trigger a stop-the-world collection.
            lua_gc(lua, LUA_GCSTEP, 96);
            auto find_free_slot = [this]() -> int {
                for (size_t index = 0; index < large_image_pool.size(); ++index) {
                    if (!large_image_pool[index].in_use) {
                        return static_cast<int>(index);
                    }
                }
                return -1;
            };
            int slot_index = find_free_slot();
            if (slot_index < 0) {
                lua_gc(lua, LUA_GCCOLLECT);
                slot_index = find_free_slot();
            }
            if (slot_index >= 0) {
                LargeImageSlot& slot = large_image_pool[slot_index];
                if (slot.capacity < bytes) {
                    void* replacement = heap_caps_realloc(
                        slot.pixels, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (!replacement) {
                        replacement = heap_caps_realloc(
                            slot.pixels, bytes, MALLOC_CAP_8BIT);
                    }
                    if (replacement) {
                        slot.pixels = static_cast<uint8_t*>(replacement);
                        slot.capacity = bytes;
                    }
                }
                if (slot.pixels && slot.capacity >= bytes) {
                    Image* image = pushImage();
                    image->width = width;
                    image->height = height;
                    image->stride = width;
                    image->pixels = slot.pixels;
                    image->pooled_slot = static_cast<int8_t>(slot_index);
                    slot.in_use = true;
                    std::memset(image->pixels, color, bytes);
                    setSolidImageBounds(*image, color);
                    return image;
                }
            }
        }
        auto* image = static_cast<Image*>(lua_newuserdatauv(
            lua, sizeof(Image) + bytes, 1));
        new (image) Image{};
        image->width = width;
        image->height = height;
        image->stride = width;
        image->pixels = reinterpret_cast<uint8_t*>(image + 1);
        image->owns_pixels = false;
        std::memset(image->pixels, color, bytes);
        setSolidImageBounds(*image, color);
        luaL_getmetatable(lua, kImageMetatable);
        lua_setmetatable(lua, -2);
        return image;
    }

    static int cImageGc(lua_State* state) {
        Impl* runtime = self(state);
        auto* image = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        if (image && image->pooled_slot >= 0 && runtime) {
            const size_t slot_index = static_cast<size_t>(image->pooled_slot);
            if (slot_index < runtime->large_image_pool.size()) {
                runtime->large_image_pool[slot_index].in_use = false;
            }
            *image = {};
        } else if (image) {
            releaseImage(*image);
        }
        return 0;
    }

    uint8_t imagePixel(const Image& image, int x, int y) const {
        if (x < 0 || y < 0 || x >= image.width || y >= image.height) return Clear;
        uint8_t value = Clear;
        if (image.pixels) {
            value = image.pixels[static_cast<size_t>(y) * image.stride + x];
            if (image.inverted) {
                if (value == Black) value = White;
                else if (value == White) value = Black;
            }
            return value;
        }
        if (!image.asset) return Clear;
        const int columns = std::max(1, image.asset->sheet_width /
                                         image.asset->frame_width);
        const int origin_x = (image.frame % columns) * image.asset->frame_width;
        const int origin_y = (image.frame / columns) * image.asset->frame_height;
        value = celesteAssetPixel(*image.asset, origin_x + x, origin_y + y);
        if (image.inverted) {
            if (value == Black) value = White;
            else if (value == White) value = Black;
        }
        return value;
    }

    uint8_t targetPixel(int x, int y) const {
        if (!target || !target->pixels || x < 0 || y < 0 ||
            x >= target->width || y >= target->height) return Clear;
        return target->pixels[static_cast<size_t>(y) * target->stride + x];
    }

    void setTargetPixelRaw(int x, int y, uint8_t color) {
        if (!target || !target->pixels || x < 0 || y < 0 ||
            x >= target->width || y >= target->height) return;
        target->pixels[static_cast<size_t>(y) * target->stride + x] = color;
        if (target != &screen && color != Clear) {
            expandContentBounds(*target, x, y, x + 1, y + 1);
            if (color == Black) {
                expandBlackBounds(*target, x, y, x + 1, y + 1);
            }
        }
    }

    uint8_t mappedColor(uint8_t source, int destination_x, int destination_y) const {
        if (source == Clear) return Clear;
        if (source == Pattern) {
            const unsigned row = static_cast<unsigned>(
                destination_y - pattern_offset_y) & 7U;
            const unsigned column = static_cast<unsigned>(
                destination_x - pattern_offset_x) & 7U;
            if (draw_pattern_uses_image) {
                source = draw_pattern_pixels[row * 8U + column];
                if (source == Clear) return Clear;
            } else {
            const bool set =
                (draw_pattern[row] & (0x80U >> column)) != 0U;
            if (draw_pattern_transparent) {
                if (set) source = draw_pattern_color;
                else source = Clear;
            } else {
                source = set ? Black : White;
            }
            }
        }
        if (source == Xor) {
            return targetPixel(destination_x, destination_y) == Black
                ? White : Black;
        }
        if (draw_mode == WhiteTransparent) {
            return source == White ? static_cast<uint8_t>(Clear) : source;
        }
        if (draw_mode == BlackTransparent) {
            return source == Black ? static_cast<uint8_t>(Clear) : source;
        }
        if (draw_mode == FillWhite) return White;
        if (draw_mode == FillBlack) return Black;
        if (draw_mode == Inverted) return source == Black ? White : Black;
        if (draw_mode == DrawXor) {
            if (source != White) return Clear;
            return targetPixel(destination_x, destination_y) == Black
                ? White : Black;
        }
        if (draw_mode == Nxor) {
            if (source != Black) return Clear;
            return targetPixel(destination_x, destination_y) == Black ? White : Black;
        }
        return source;
    }

    void putLogicalPixel(int x, int y, uint8_t source) {
        if (!target || !clip.contains(x, y)) return;
        if (target == &screen) {
            if (stencil && imagePixel(*stencil, x, y) == Black) return;
            const uint8_t value = mappedColor(source, x, y);
            if (value != Clear) {
                const uint8_t output = inverted_display
                    ? static_cast<uint8_t>(value == Black ? White : Black)
                    : value;
                setTargetPixelRaw(x, y, output);
            }
            return;
        }
        if (stencil && imagePixel(*stencil, x, y) == Black) return;
        const uint8_t value = mappedColor(source, x, y);
        if (value != Clear || source == Clear) setTargetPixelRaw(x, y, value);
    }

    void drawImage(const Image& image, int x, int y, int flip,
                   int scale = 1, float fade = 1.0f) {
        scale = std::max(1, scale);
        int content_left = 0, content_top = 0;
        int content_right = image.width, content_bottom = image.height;
        logicalContentBounds(image, flip, content_left, content_top,
                             content_right, content_bottom,
                             draw_mode == WhiteTransparent ||
                                 draw_mode == Nxor);
        if (content_left >= content_right || content_top >= content_bottom) {
            return;
        }
        // Celeste composites several persistent 128x128 layer images every
        // frame. Copy their byte pixels row-wise into the logical framebuffer
        // instead of routing every pixel through the generic scaled path.
        if (scale == 1 && fade >= 0.999f && image.pixels && !image.inverted &&
            !image.mask &&
            !stencil &&
            (draw_mode == Copy || draw_mode == FillWhite ||
             draw_mode == FillBlack || draw_mode == Inverted) &&
            target && target->pixels) {
            const int start_x = std::max(content_left,
                std::max(clip.x - x, -x));
            const int start_y = std::max(content_top,
                std::max(clip.y - y, -y));
            const int end_x = std::min(content_right,
                std::min(clip.x + clip.w - x, target->width - x));
            const int end_y = std::min(content_bottom,
                std::min(clip.y + clip.h - y, target->height - y));
            if (target != &screen && start_x < end_x && start_y < end_y) {
                expandContentBounds(*target, x + start_x, y + start_y,
                                    x + end_x, y + end_y);
            }
            for (int dy = start_y; dy < end_y; ++dy) {
                const int source_y = (flip & FlippedY)
                    ? image.height - 1 - dy : dy;
                const uint8_t* source_row = image.pixels +
                    static_cast<size_t>(source_y) * image.stride;
                uint8_t* destination_row = target->pixels +
                    static_cast<size_t>(y + dy) * target->stride;

                // The persistent terrain/cache layers use ordinary Copy mode,
                // no flip and byte pixels where 0 is transparent.  Handle
                // four pixels per load: fully transparent groups are skipped
                // and fully opaque groups become one memcpy.  This removes
                // the per-pixel flip/draw-mode/inversion branches from the
                // hottest remaining Celeste renderer path, especially when
                // the layer pixels live in PSRAM.
                const bool plain_copy = flip == Unflipped && draw_mode == Copy &&
                    (target != &screen || !inverted_display);
                if (plain_copy) {
                    int dx = start_x;
                    for (; dx + 4 <= end_x; dx += 4) {
                        uint32_t group = 0;
                        std::memcpy(&group, source_row + dx, sizeof(group));
                        if (group == 0U) continue;
                        const bool has_clear = ((group - 0x01010101U) &
                            ~group & 0x80808080U) != 0U;
                        if (!has_clear) {
                            std::memcpy(destination_row + x + dx,
                                        &group, sizeof(group));
                            continue;
                        }
                        for (int index = 0; index < 4; ++index) {
                            const uint8_t value = source_row[dx + index];
                            if (value != Clear) destination_row[x + dx + index] = value;
                        }
                    }
                    for (; dx < end_x; ++dx) {
                        const uint8_t value = source_row[dx];
                        if (value != Clear) destination_row[x + dx] = value;
                    }
                    continue;
                }

                for (int dx = start_x; dx < end_x; ++dx) {
                    const int source_x = (flip & FlippedX)
                        ? image.width - 1 - dx : dx;
                    uint8_t value = source_row[source_x];
                    if (value == Clear) continue;
                    if (draw_mode == FillWhite) value = White;
                    else if (draw_mode == FillBlack) value = Black;
                    else if (draw_mode == Inverted)
                        value = value == Black ? White : Black;
                    if (target == &screen && inverted_display)
                        value = value == Black ? White : Black;
                    destination_row[x + dx] = value;
                }
            }
            return;
        }
        static constexpr uint8_t bayer[16] = {
            0, 8, 2, 10, 12, 4, 14, 6,
            3, 11, 1, 9, 15, 7, 13, 5,
        };
        const int threshold = std::clamp(static_cast<int>(fade * 16.0f), 0, 16);
        for (int sy = content_top; sy < content_bottom; ++sy) {
            for (int sx = content_left; sx < content_right; ++sx) {
                if (bayer[((y + sy) & 3) * 4 + ((x + sx) & 3)] >= threshold) continue;
                const int source_x = (flip & FlippedX) ? image.width - 1 - sx : sx;
                const int source_y = (flip & FlippedY) ? image.height - 1 - sy : sy;
                if (image.mask &&
                    imagePixel(*image.mask, source_x, source_y) != White) continue;
                const uint8_t value = imagePixel(image, source_x, source_y);
                if (value == Clear) continue;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        putLogicalPixel(x + sx * scale + dx,
                                        y + sy * scale + dy, value);
                    }
                }
            }
        }
    }

    static int scaledExtent(int source_extent, float scale) {
        if (source_extent <= 0 || !std::isfinite(scale) || scale < 0.0f) {
            return 0;
        }
        // A positive scale smaller than one source pixel still produces a
        // valid one-pixel image.  Maze animates its marble all the way down
        // to zero while falling into a hole and expects scaledImage() to keep
        // returning an image until the animator reports that it has ended.
        return std::clamp(static_cast<int>(std::lround(
            static_cast<float>(source_extent) * scale)), 1, 1024);
    }

    uint8_t scaledImagePixel(const Image& image, int destination_x,
                             int destination_y, int destination_width,
                             int destination_height,
                             int flip = Unflipped) const {
        int source_x = std::min(image.width - 1,
            (destination_x * image.width) / destination_width);
        int source_y = std::min(image.height - 1,
            (destination_y * image.height) / destination_height);
        if (flip & FlippedX) source_x = image.width - 1 - source_x;
        if (flip & FlippedY) source_y = image.height - 1 - source_y;
        if (image.mask && imagePixel(*image.mask, source_x, source_y) != White) {
            return Clear;
        }
        return imagePixel(image, source_x, source_y);
    }

    void drawImageScaled(const Image& image, int x, int y,
                         float scale_x, float scale_y,
                         int flip = Unflipped) {
        const int destination_width = scaledExtent(image.width, scale_x);
        const int destination_height = scaledExtent(image.height, scale_y);
        if (destination_width <= 0 || destination_height <= 0) return;
        int content_left = 0, content_top = 0;
        int content_right = image.width, content_bottom = image.height;
        logicalContentBounds(image, flip, content_left, content_top,
                             content_right, content_bottom,
                             draw_mode == WhiteTransparent ||
                                 draw_mode == Nxor);
        if (content_left >= content_right || content_top >= content_bottom) {
            return;
        }

        // Scaled Duel sprites are almost always enlarged (1.5x/1.75x).  The
        // destination-driven path below rereads a PSRAM-backed source pixel
        // two to four times in that case.  Walk the source once and expand
        // each nearest-neighbour cell instead.  The ceil-divided ranges are
        // exactly the inverse of floor(dst * source / destination), so this
        // is pixel-identical to the Playdate-style nearest-neighbour result.
        const int64_t destination_area =
            static_cast<int64_t>(destination_width) * destination_height;
        const int64_t source_area =
            static_cast<int64_t>(image.width) * image.height;
        if (destination_area >= source_area) {
            const auto ceilDivide = [](int64_t numerator, int denominator) {
                return static_cast<int>((numerator + denominator - 1) /
                                        denominator);
            };
            for (int logical_source_y = content_top;
                 logical_source_y < content_bottom;
                 ++logical_source_y) {
                const int destination_y_begin = ceilDivide(
                    static_cast<int64_t>(logical_source_y) *
                        destination_height,
                    image.height);
                const int destination_y_end = ceilDivide(
                    static_cast<int64_t>(logical_source_y + 1) *
                        destination_height,
                    image.height);
                const int source_y = (flip & FlippedY)
                    ? image.height - 1 - logical_source_y
                    : logical_source_y;
                for (int logical_source_x = content_left;
                     logical_source_x < content_right;
                     ++logical_source_x) {
                    const int destination_x_begin = ceilDivide(
                        static_cast<int64_t>(logical_source_x) *
                            destination_width,
                        image.width);
                    const int destination_x_end = ceilDivide(
                        static_cast<int64_t>(logical_source_x + 1) *
                            destination_width,
                        image.width);
                    const int source_x = (flip & FlippedX)
                        ? image.width - 1 - logical_source_x
                        : logical_source_x;
                    if (image.mask &&
                        imagePixel(*image.mask, source_x, source_y) != White) {
                        continue;
                    }
                    const uint8_t value = imagePixel(image, source_x, source_y);
                    if (value == Clear) continue;
                    for (int destination_y = destination_y_begin;
                         destination_y < destination_y_end; ++destination_y) {
                        for (int destination_x = destination_x_begin;
                             destination_x < destination_x_end;
                             ++destination_x) {
                            putLogicalPixel(x + destination_x,
                                            y + destination_y, value);
                        }
                    }
                }
            }
            return;
        }
        const auto ceilDivide = [](int64_t numerator, int denominator) {
            return static_cast<int>((numerator + denominator - 1) /
                                    denominator);
        };
        const int destination_left = ceilDivide(
            static_cast<int64_t>(content_left) * destination_width,
            image.width);
        const int destination_top = ceilDivide(
            static_cast<int64_t>(content_top) * destination_height,
            image.height);
        const int destination_right = ceilDivide(
            static_cast<int64_t>(content_right) * destination_width,
            image.width);
        const int destination_bottom = ceilDivide(
            static_cast<int64_t>(content_bottom) * destination_height,
            image.height);
        for (int destination_y = destination_top;
             destination_y < destination_bottom;
             ++destination_y) {
            for (int destination_x = destination_left;
                 destination_x < destination_right;
                 ++destination_x) {
                const uint8_t value = scaledImagePixel(
                    image, destination_x, destination_y,
                    destination_width, destination_height, flip);
                if (value != Clear) {
                    putLogicalPixel(x + destination_x, y + destination_y, value);
                }
            }
        }
    }

    void drawImageRotated(const Image& image, float center_x, float center_y,
                          float angle_degrees, float scale_x, float scale_y) {
        if (!target || !target->pixels || image.width <= 0 || image.height <= 0 ||
            !std::isfinite(center_x) || !std::isfinite(center_y) ||
            !std::isfinite(angle_degrees) || !std::isfinite(scale_x) ||
            !std::isfinite(scale_y) || std::fabs(scale_x) < 0.0001f ||
            std::fabs(scale_y) < 0.0001f) {
            return;
        }

        // Playdate rotates clockwise around the image centre.  Coordinates on
        // the LCD have a downward-pointing Y axis, so the ordinary 2D rotation
        // matrix already has the required clockwise visual direction.
        float normalized = std::fmod(angle_degrees, 360.0f);
        if (normalized < 0.0f) normalized += 360.0f;
        // Several animation helpers call drawRotated(..., 0, scale) even
        // though no rotation is requested.  Avoid the inverse-transform path
        // for that common case; on ESP32-S3 this is especially important for
        // Duel's scaled boss and VFX sprites.
        if ((normalized < 0.0001f ||
             std::fabs(normalized - 360.0f) < 0.0001f) &&
            scale_x > 0.0f && scale_y > 0.0f) {
            const int destination_width = scaledExtent(image.width, scale_x);
            const int destination_height = scaledExtent(image.height, scale_y);
            if (destination_width > 0 && destination_height > 0) {
                const int left = static_cast<int>(std::floor(
                    center_x - static_cast<float>(destination_width) * 0.5f));
                const int top = static_cast<int>(std::floor(
                    center_y - static_cast<float>(destination_height) * 0.5f));
                drawImageScaled(image, left, top, scale_x, scale_y);
            }
            return;
        }
        const float quadrant = std::round(normalized / 90.0f);
        float cosine = 0.0f;
        float sine = 0.0f;
        const bool exact_quadrant =
            std::fabs(normalized - quadrant * 90.0f) < 0.0001f ||
            std::fabs(normalized - 360.0f) < 0.0001f;
        if (exact_quadrant) {
            switch (static_cast<int>(quadrant) & 3) {
                case 0: cosine = 1.0f; sine = 0.0f; break;
                case 1: cosine = 0.0f; sine = 1.0f; break;
                case 2: cosine = -1.0f; sine = 0.0f; break;
                default: cosine = 0.0f; sine = -1.0f; break;
            }
        } else {
            constexpr float radians_per_degree =
                3.14159265358979323846f / 180.0f;
            const float radians = normalized * radians_per_degree;
            cosine = std::cos(radians);
            sine = std::sin(radians);
        }

        // At 1x, quarter turns map every source pixel to exactly one output
        // pixel.  Forward-map that special case so even-sized images keep all
        // their edge pixels instead of landing on an inverse-sampling tie.
        if (exact_quadrant && std::fabs(std::fabs(scale_x) - 1.0f) < 0.0001f &&
            std::fabs(std::fabs(scale_y) - 1.0f) < 0.0001f) {
            const float half_width = static_cast<float>(image.width) * 0.5f;
            const float half_height = static_cast<float>(image.height) * 0.5f;
            for (int source_y = 0; source_y < image.height; ++source_y) {
                const float local_y =
                    (source_y + 0.5f - half_height) * scale_y;
                for (int source_x = 0; source_x < image.width; ++source_x) {
                    if (image.mask &&
                        imagePixel(*image.mask, source_x, source_y) != White) continue;
                    const uint8_t value = imagePixel(image, source_x, source_y);
                    if (value == Clear) continue;
                    const float local_x =
                        (source_x + 0.5f - half_width) * scale_x;
                    const int destination_x = static_cast<int>(std::floor(
                        center_x + cosine * local_x - sine * local_y));
                    const int destination_y = static_cast<int>(std::floor(
                        center_y + sine * local_x + cosine * local_y));
                    putLogicalPixel(destination_x, destination_y, value);
                }
            }
            return;
        }

        // Transform the four source cell-edge corners to obtain the exact
        // destination AABB, then clip before rasterization.  Clipping here is
        // important when a game rotates a large image mostly off-screen.
        const float half_width = static_cast<float>(image.width) * 0.5f;
        const float half_height = static_cast<float>(image.height) * 0.5f;
        float minimum_x = center_x;
        float maximum_x = center_x;
        float minimum_y = center_y;
        float maximum_y = center_y;
        bool first_corner = true;
        for (int y_sign : {-1, 1}) {
            for (int x_sign : {-1, 1}) {
                const float source_x = x_sign * half_width * scale_x;
                const float source_y = y_sign * half_height * scale_y;
                const float destination_x = center_x +
                    cosine * source_x - sine * source_y;
                const float destination_y = center_y +
                    sine * source_x + cosine * source_y;
                if (first_corner) {
                    minimum_x = maximum_x = destination_x;
                    minimum_y = maximum_y = destination_y;
                    first_corner = false;
                } else {
                    minimum_x = std::min(minimum_x, destination_x);
                    maximum_x = std::max(maximum_x, destination_x);
                    minimum_y = std::min(minimum_y, destination_y);
                    maximum_y = std::max(maximum_y, destination_y);
                }
            }
        }

        const int clip_left = std::clamp(clip.x, 0, target->width);
        const int clip_top = std::clamp(clip.y, 0, target->height);
        const int64_t raw_clip_right =
            static_cast<int64_t>(clip.x) + static_cast<int64_t>(clip.w);
        const int64_t raw_clip_bottom =
            static_cast<int64_t>(clip.y) + static_cast<int64_t>(clip.h);
        const int clip_right = static_cast<int>(std::clamp<int64_t>(
            raw_clip_right, 0, target->width));
        const int clip_bottom = static_cast<int>(std::clamp<int64_t>(
            raw_clip_bottom, 0, target->height));
        if (clip_left >= clip_right || clip_top >= clip_bottom) return;

        minimum_x = std::max(minimum_x, static_cast<float>(clip_left));
        minimum_y = std::max(minimum_y, static_cast<float>(clip_top));
        maximum_x = std::min(maximum_x, static_cast<float>(clip_right));
        maximum_y = std::min(maximum_y, static_cast<float>(clip_bottom));
        if (minimum_x >= maximum_x || minimum_y >= maximum_y) return;
        constexpr float edge_epsilon = 0.0001f;
        const int start_x = static_cast<int>(std::ceil(minimum_x - edge_epsilon));
        const int start_y = static_cast<int>(std::ceil(minimum_y - edge_epsilon));
        const int end_x = static_cast<int>(std::ceil(maximum_x - edge_epsilon));
        const int end_y = static_cast<int>(std::ceil(maximum_y - edge_epsilon));
        if (start_x >= end_x || start_y >= end_y) return;

        // Inverse-map each destination pixel.  Nearest-neighbour sampling is
        // the correct choice for Playdate's 1-bit pixel art and avoids holes
        // that forward-mapping would leave at off-axis angles.
        const float x_step_source_x = cosine / scale_x;
        const float x_step_source_y = -sine / scale_y;
        const float y_step_source_x = sine / scale_x;
        const float y_step_source_y = cosine / scale_y;
        float row_source_x =
            (cosine * (start_x - center_x) +
             sine * (start_y - center_y)) / scale_x + half_width;
        float row_source_y =
            (-sine * (start_x - center_x) +
             cosine * (start_y - center_y)) / scale_y + half_height;

        for (int destination_y = start_y; destination_y < end_y; ++destination_y) {
            float source_x = row_source_x;
            float source_y = row_source_y;
            for (int destination_x = start_x; destination_x < end_x;
                 ++destination_x) {
                if (source_x >= 0.0f && source_y >= 0.0f &&
                    source_x < image.width && source_y < image.height) {
                    const int sample_x = static_cast<int>(std::floor(source_x));
                    const int sample_y = static_cast<int>(std::floor(source_y));
                    if (image.mask &&
                        imagePixel(*image.mask, sample_x, sample_y) != White) {
                        source_x += x_step_source_x;
                        source_y += x_step_source_y;
                        continue;
                    }
                    const uint8_t value = imagePixel(image, sample_x, sample_y);
                    if (value != Clear) {
                        putLogicalPixel(destination_x, destination_y, value);
                    }
                }
                source_x += x_step_source_x;
                source_y += x_step_source_y;
            }
            row_source_x += y_step_source_x;
            row_source_y += y_step_source_y;
        }
    }

    void fillTarget(uint8_t color) {
        if (!target || !target->pixels) return;
        if (color == Xor) {
            const size_t pixels = static_cast<size_t>(target->stride) * target->height;
            for (size_t index = 0; index < pixels; ++index) {
                uint8_t& value = target->pixels[index];
                if (value == Black) value = White;
                else if (value == White) value = Black;
            }
            return;
        }
        if (target == &screen) {
            const uint8_t value = color == Clear ? background_color : color;
            std::memset(screen.pixels, value,
                        static_cast<size_t>(screen.width) * screen.height);
            return;
        }
        std::memset(target->pixels, color,
                    static_cast<size_t>(target->stride) * target->height);
        setSolidImageBounds(*target, color);
    }

    void resetTargetToScreen() {
        target = &screen;
        clip = {0, 0, screen.width, screen.height};
        stencil = nullptr;
        context_depth = 0;
    }

    void beginFrame() {
        resetTargetToScreen();
        draw_mode = Copy;
        draw_color = Black;
        solid_draw_color = Black;
        fillTarget(background_color);
    }

    void flushScreen() {
        if (!canvas || !screen.pixels) return;
        const int output_x = display_offset_x + draw_offset_x;
        const int output_y = display_offset_y + draw_offset_y;
        if (output_x != 0 || output_y != 0) {
            canvas->clear(background_color == Black ? gfx::BLACK : gfx::WHITE);
        }
        canvas->reset_clip();
        canvas->draw_indexed2_fast(
            output_x, output_y, screen.width, screen.height,
            screen.pixels, screen.width * display_scale,
            screen.height * display_scale, false, false);
    }

    static uint16_t readLe16(const uint8_t* value) {
        return static_cast<uint16_t>(value[0] |
            (static_cast<uint16_t>(value[1]) << 8U));
    }

    static uint32_t readLe32(const uint8_t* value) {
        return static_cast<uint32_t>(value[0]) |
            (static_cast<uint32_t>(value[1]) << 8U) |
            (static_cast<uint32_t>(value[2]) << 16U) |
            (static_cast<uint32_t>(value[3]) << 24U);
    }

    static uint32_t readLe24(const uint8_t* value) {
        return static_cast<uint32_t>(value[0]) |
            (static_cast<uint32_t>(value[1]) << 8U) |
            (static_cast<uint32_t>(value[2]) << 16U);
    }

    static uint8_t checkPlaydateColor(lua_State* state, int index,
                                      uint8_t default_color) {
        if (lua_isnoneornil(state, index)) return default_color;
        switch (luaL_checkinteger(state, index)) {
            case PdColorBlack: return Black;
            case PdColorWhite: return White;
            case PdColorClear: return Clear;
            case PdColorXor: return Xor;
            default:
                luaL_argerror(state, index,
                    "color must be kColorBlack, kColorWhite, kColorClear or kColorXOR");
                return default_color;
        }
    }

    bool resourcePath(char* output, size_t capacity, const char* requested,
                      const char* extension) const {
        if (!package_mode || !requested || !requested[0]) return false;
        char relative[192]{};
        const char* start = requested;
        while (*start == '/') ++start;
        if (!start[0] || std::strstr(start, "..") ||
            std::strlen(start) >= sizeof(relative)) return false;
        std::snprintf(relative, sizeof(relative), "%s", start);

        // Lua source refers to authoring files (for example
        // foo-table-16-16.png), while a compiled PDX contains foo.pdt.  The
        // Playdate loader performs this source-to-compiled extension mapping.
        const char* source_extensions[] = {".png", ".gif", ".bmp", ".fnt"};
        size_t length = std::strlen(relative);
        for (const char* source_extension : source_extensions) {
            const size_t source_length = std::strlen(source_extension);
            if (length >= source_length &&
                std::strcmp(relative + length - source_length,
                            source_extension) == 0) {
                relative[length - source_length] = '\0';
                length -= source_length;
                break;
            }
        }
        if (std::strcmp(extension, ".pdt") == 0) {
            char* table_suffix = std::strstr(relative, "-table-");
            if (table_suffix) {
                const char* cursor = table_suffix + 7;
                bool first_number = false;
                while (*cursor >= '0' && *cursor <= '9') {
                    first_number = true;
                    ++cursor;
                }
                if (first_number && *cursor++ == '-') {
                    bool second_number = false;
                    while (*cursor >= '0' && *cursor <= '9') {
                        second_number = true;
                        ++cursor;
                    }
                    if (second_number && *cursor == '\0') *table_suffix = '\0';
                }
            }
        }
        length = std::strlen(relative);
        const size_t extension_length = std::strlen(extension);
        const bool has_extension = length >= extension_length &&
            std::strcmp(relative + length - extension_length, extension) == 0;
        return pdxJoinPath(output, capacity, package_info.path, relative,
                           has_extension ? nullptr : extension);
    }

    bool loadCompiledResource(const char* requested, const char* extension,
                              const char* magic, uint8_t*& unpacked,
                              size_t& unpacked_size, uint32_t& width,
                              uint32_t& height, uint32_t& count) {
        unpacked = nullptr; unpacked_size = 0; width = height = count = 0;
        char path[384]{};
        if (!resourcePath(path, sizeof(path), requested, extension)) return false;
        FILE* file = std::fopen(path, "rb");
        if (!file) return false;
        std::fseek(file, 0, SEEK_END);
        const long length = std::ftell(file);
        std::rewind(file);
        uint8_t header[32]{};
        if (length < 32 || std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
            std::memcmp(header, magic, 12) != 0) {
            std::fclose(file);
            return false;
        }
        const uint32_t target_size = readLe32(header + 16);
        width = readLe32(header + 20);
        height = readLe32(header + 24);
        count = readLe32(header + 28);
        if (!(header[15] & 0x80U) || target_size < 16U ||
            target_size > 4U * 1024U * 1024U || width == 0 || height == 0 ||
            width > 1024U || height > 1024U) {
            std::fclose(file);
            return false;
        }
        const size_t packed_size = static_cast<size_t>(length - 32);
        if (packed_size == 0 || packed_size > 4U * 1024U * 1024U) {
            std::fclose(file);
            return false;
        }
        uint8_t* packed = static_cast<uint8_t*>(heap_caps_malloc(
            packed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        uint8_t* output = static_cast<uint8_t*>(heap_caps_malloc(
            target_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if ((!packed || !output) && packed_size < 128U * 1024U && target_size < 128U * 1024U) {
            if (!packed) packed = static_cast<uint8_t*>(heap_caps_malloc(packed_size, MALLOC_CAP_8BIT));
            if (!output) output = static_cast<uint8_t*>(heap_caps_malloc(target_size, MALLOC_CAP_8BIT));
        }
        if (!packed || !output || std::fread(packed, 1, packed_size, file) != packed_size) {
            if (packed) heap_caps_free(packed);
            if (output) heap_caps_free(output);
            std::fclose(file);
            return false;
        }
        std::fclose(file);
        uLongf actual = target_size;
        const int zerr = uncompress(output, &actual, packed, packed_size);
        heap_caps_free(packed);
        if (zerr != Z_OK || actual != target_size) {
            heap_caps_free(output);
            return false;
        }
        unpacked = output;
        unpacked_size = target_size;
        return true;
    }

    bool decodeSerializedImage(Image& image, const uint8_t* bytes, size_t size,
                               int full_width, int full_height) {
        if (!bytes || size < 16U || !image.pixels ||
            image.width != full_width || image.height != full_height) return false;
        const uint16_t stored_width = readLe16(bytes);
        const uint16_t stored_height = readLe16(bytes + 2);
        const uint16_t row_bytes = readLe16(bytes + 4);
        const uint16_t left = readLe16(bytes + 6);
        const uint16_t top = readLe16(bytes + 10);
        const uint16_t flags = readLe16(bytes + 14);
        const size_t plane_size = static_cast<size_t>(row_bytes) * stored_height;
        // Playdate treats either of the low transparency bits as a masked
        // cell.  Requiring both happened to work for the first Celeste
        // assets, but is too strict for compiled fonts and other PDX files.
        const bool has_mask = (flags & 0x03U) != 0U;
        const size_t plane_count = has_mask ? 2U : 1U;
        if (stored_width == 0 || stored_height == 0 || row_bytes == 0 ||
            stored_width > row_bytes * 8U || left + stored_width > full_width ||
            top + stored_height > full_height ||
            plane_size > (size - 16U) / plane_count) return false;
        const uint8_t* bitmap = bytes + 16U;
        const uint8_t* mask = has_mask ? bitmap + plane_size : nullptr;
        std::memset(image.pixels, Clear,
                    static_cast<size_t>(image.stride) * image.height);
        int content_left = full_width;
        int content_top = full_height;
        int content_right = 0;
        int content_bottom = 0;
        int black_left = full_width;
        int black_top = full_height;
        int black_right = 0;
        int black_bottom = 0;
        for (uint16_t y = 0; y < stored_height; ++y) {
            for (uint16_t x = 0; x < stored_width; ++x) {
                const uint8_t bit = static_cast<uint8_t>(0x80U >> (x & 7U));
                const size_t index = static_cast<size_t>(y) * row_bytes + (x >> 3U);
                if (mask && !(mask[index] & bit)) continue;
                const int destination_x = left + x;
                const int destination_y = top + y;
                const uint8_t value = (bitmap[index] & bit) ? White : Black;
                image.pixels[static_cast<size_t>(destination_y) *
                    image.stride + destination_x] = value;
                content_left = std::min(content_left, destination_x);
                content_top = std::min(content_top, destination_y);
                content_right = std::max(content_right, destination_x + 1);
                content_bottom = std::max(content_bottom, destination_y + 1);
                if (value == Black) {
                    black_left = std::min(black_left, destination_x);
                    black_top = std::min(black_top, destination_y);
                    black_right = std::max(black_right, destination_x + 1);
                    black_bottom = std::max(black_bottom, destination_y + 1);
                }
            }
        }
        setContentBounds(image, content_right, content_bottom,
                         content_left == full_width ? 0 : content_left,
                         content_top == full_height ? 0 : content_top);
        setBlackBounds(image, black_right, black_bottom,
                       black_left == full_width ? 0 : black_left,
                       black_top == full_height ? 0 : black_top);
        return true;
    }

    bool pushPdiImage(const char* path) {
        uint8_t* bytes = nullptr; size_t size = 0;
        uint32_t width = 0, height = 0, unused = 0;
        if (!loadCompiledResource(path, ".pdi", "Playdate IMG", bytes, size,
                                  width, height, unused)) return false;
        Image* image = pushDynamicImage(static_cast<int>(width),
                                        static_cast<int>(height), Clear);
        const bool ok = image && decodeSerializedImage(*image, bytes, size,
            static_cast<int>(width), static_cast<int>(height));
        heap_caps_free(bytes);
        if (!ok) {
            if (image) lua_pop(lua, 1);
            return false;
        }
        return true;
    }

    bool pushPdtTable(const char* path) {
        uint8_t* bytes = nullptr; size_t size = 0;
        uint32_t width = 0, height = 0, count = 0;
        if (!loadCompiledResource(path, ".pdt", "Playdate IMT", bytes, size,
                                  width, height, count)) return false;
        if (count == 0 || count > 2048U || size < 4U + count * 4U ||
            readLe16(bytes) != count) {
            heap_caps_free(bytes);
            return false;
        }
        auto* table = static_cast<ImageTable*>(lua_newuserdatauv(
            lua, sizeof(ImageTable) + size, 1));
        new (table) ImageTable{};
        table->data = reinterpret_cast<uint8_t*>(table + 1);
        table->data_size = size;
        table->frame_width = static_cast<uint16_t>(width);
        table->frame_height = static_cast<uint16_t>(height);
        table->frame_count = static_cast<uint16_t>(count);
        std::memcpy(table->data, bytes, size);
        heap_caps_free(bytes);
        luaL_getmetatable(lua, kImageTableMetatable);
        lua_setmetatable(lua, -2);
        lua_newtable(lua);
        lua_setiuservalue(lua, -2, 1);
        return true;
    }

    static int cImageNew(lua_State* state) {
        Impl* runtime = self(state);
        if (lua_type(state, 1) == LUA_TSTRING) {
            const char* path = lua_tostring(state, 1);
            if (runtime->pushPdiImage(path)) return 1;
            const CelesteAsset* asset = findCelesteAsset(path);
            if (asset) {
                Image* image = runtime->pushImage();
                image->width = asset->frame_width;
                image->height = asset->frame_height;
                image->asset = asset;
                return 1;
            }
            if (path && (std::strstr(path, "snake") || std::strstr(path, "qrcode"))) {
                const bool qr = std::strstr(path, "qrcode") != nullptr;
                Image* image = runtime->pushDynamicImage(qr ? 88 : 26, qr ? 88 : 26, Clear);
                if (!image) return 1;
                if (qr) {
                    for (int row = 0; row < 21; ++row) {
                        for (int column = 0; column < 21; ++column) {
                            const bool edge = ((column < 7 && row < 7) ||
                                (column >= 14 && row < 7) || (column < 7 && row >= 14)) &&
                                (column % 6 == 0 || row % 6 == 0 ||
                                 (column % 6 >= 2 && column % 6 <= 4 &&
                                  row % 6 >= 2 && row % 6 <= 4));
                            const bool data = ((column * 17 + row * 31 + column * row * 7) & 3) == 0;
                            if (!edge && !data) continue;
                            for (int yy = 0; yy < 4; ++yy) for (int xx = 0; xx < 4; ++xx) {
                                const int px = 2 + column * 4 + xx;
                                const int py = 2 + row * 4 + yy;
                                if (px < image->width && py < image->height)
                                    image->pixels[py * image->stride + px] = Black;
                            }
                        }
                    }
                } else {
                    for (int y = 2; y < 24; ++y) for (int x = 2; x < 24; ++x) {
                        const bool body = (x-8)*(x-8)+(y-8)*(y-8) < 40 ||
                                          (x >= 8 && x < 22 && y >= 7 && y < 15) ||
                                          (x >= 14 && x < 22 && y >= 13 && y < 22);
                        if (body) image->pixels[y * image->stride + x] = Black;
                    }
                    image->pixels[5 * image->stride + 6] = White;
                    image->pixels[5 * image->stride + 10] = White;
                }
                setContentBounds(*image, image->width, image->height);
                setBlackBounds(*image, image->width, image->height);
                return 1;
            }
            lua_pushnil(state);
            return 1;
        }
        // Playdate's Lua bridge coerces numeric dimensions to the integer C
        // image API.  Games commonly derive hit-box images from scaled sprite
        // sizes, which can leave a fractional Lua number even when the final
        // pixel extent is unambiguous.
        const int width = static_cast<int>(std::lround(
            luaL_checknumber(state, 1)));
        const int height = static_cast<int>(std::lround(
            luaL_checknumber(state, 2)));
        const uint8_t color = checkPlaydateColor(state, 3, Clear);
        // Maze's LevelComplete object creates an identically-sized transparent
        // render target every update and immediately replaces the previous
        // sprite image.  A PSRAM plane pool avoids the 44 KiB pixel allocation,
        // but still creates one userdata and advances the collector per frame.
        // Pin and clear this exact package-owned target instead.  The game
        // redraws all of its letters after pushContext(), so reusing the object
        // is equivalent to a fresh transparent image without per-frame GC.
        const bool maze_completion_target =
            runtime->package_mode && color == Clear && width == 324 &&
            height == 137 &&
            std::strcmp(runtime->package_info.bundle_id,
                        "de.WuffderHundeheld.Maze") == 0;
        if (maze_completion_target &&
            runtime->maze_completion_image_ref != LUA_NOREF) {
            lua_rawgeti(state, LUA_REGISTRYINDEX,
                        runtime->maze_completion_image_ref);
            auto* image = static_cast<Image*>(luaL_testudata(
                state, -1, kImageMetatable));
            if (image && image->pixels && image->width == width &&
                image->height == height) {
                std::memset(image->pixels, Clear,
                            static_cast<size_t>(image->stride) * image->height);
                image->inverted = false;
                image->mask = nullptr;
                setSolidImageBounds(*image, Clear);
                return 1;
            }
            lua_pop(state, 1);
            luaL_unref(state, LUA_REGISTRYINDEX,
                       runtime->maze_completion_image_ref);
            runtime->maze_completion_image_ref = LUA_NOREF;
        }
        runtime->pushDynamicImage(width, height, color);
        if (maze_completion_target &&
            luaL_testudata(state, -1, kImageMetatable)) {
            lua_pushvalue(state, -1);
            runtime->maze_completion_image_ref = luaL_ref(
                state, LUA_REGISTRYINDEX);
            if (!runtime->maze_completion_reuse_logged) {
                ESP_LOGI(TAG,
                    "Maze LevelComplete: pinned reusable 324x137 render target");
                runtime->maze_completion_reuse_logged = true;
            }
        }
        return 1;
    }

    static int cImageDraw(lua_State* state) {
        Impl* runtime = self(state);
        auto* image = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        const int x = static_cast<int>(luaL_checknumber(state, 2));
        const int y = static_cast<int>(luaL_checknumber(state, 3));
        const int flip = static_cast<int>(luaL_optinteger(state, 4, Unflipped));
        if (image) runtime->drawImage(*image, x, y, flip);
        return 0;
    }

    static int cImageDrawIgnoringOffset(lua_State* state) {
        Impl* runtime = self(state);
        auto* image = static_cast<Image*>(
            luaL_checkudata(state, 1, kImageMetatable));
        const int x = static_cast<int>(luaL_checknumber(state, 2)) -
            runtime->draw_offset_x;
        const int y = static_cast<int>(luaL_checknumber(state, 3)) -
            runtime->draw_offset_y;
        const int flip = static_cast<int>(
            luaL_optinteger(state, 4, Unflipped));
        if (image) runtime->drawImage(*image, x, y, flip);
        return 0;
    }

    static int cImageDrawCentered(lua_State* state) {
        Impl* runtime = self(state);
        auto* image = static_cast<Image*>(luaL_checkudata(
            state, 1, kImageMetatable));
        const int center_x = static_cast<int>(luaL_checknumber(state, 2));
        const int center_y = static_cast<int>(luaL_checknumber(state, 3));
        const int flip = static_cast<int>(
            luaL_optinteger(state, 4, Unflipped));
        if (image) {
            runtime->drawImage(*image, center_x - image->width / 2,
                               center_y - image->height / 2, flip);
        }
        return 0;
    }

    static int cImageDrawScaled(lua_State* state) {
        Impl* runtime = self(state);
        auto* image = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        const int x = static_cast<int>(luaL_checknumber(state, 2));
        const int y = static_cast<int>(luaL_checknumber(state, 3));
        const float scale_x = static_cast<float>(luaL_checknumber(state, 4));
        const float scale_y = static_cast<float>(luaL_optnumber(
            state, 5, scale_x));
        const int flip = static_cast<int>(luaL_optinteger(
            state, 6, Unflipped));
        if (image) {
            runtime->drawImageScaled(*image, x, y, scale_x, scale_y, flip);
        }
        return 0;
    }

    static int cImageScaledImage(lua_State* state) {
        Impl* runtime = self(state);
        auto* source = static_cast<Image*>(luaL_checkudata(
            state, 1, kImageMetatable));
        const float scale_x = static_cast<float>(luaL_checknumber(state, 2));
        const float scale_y = static_cast<float>(luaL_optnumber(
            state, 3, scale_x));
        if (!source) {
            lua_pushnil(state);
            return 1;
        }
        const int destination_width = scaledExtent(source->width, scale_x);
        const int destination_height = scaledExtent(source->height, scale_y);
        if (destination_width <= 0 || destination_height <= 0) {
            lua_pushnil(state);
            return 1;
        }
        Image* scaled = runtime->pushDynamicImage(
            destination_width, destination_height, Clear);
        if (!scaled) return 1;
        for (int y = 0; y < destination_height; ++y) {
            for (int x = 0; x < destination_width; ++x) {
                scaled->pixels[static_cast<size_t>(y) * scaled->stride + x] =
                    runtime->scaledImagePixel(*source, x, y,
                        destination_width, destination_height);
            }
        }
        int source_left = 0, source_top = 0;
        int source_right = source->width, source_bottom = source->height;
        logicalContentBounds(*source, Unflipped, source_left, source_top,
                             source_right, source_bottom);
        const auto ceilDivide = [](int64_t numerator, int denominator) {
            return static_cast<int>((numerator + denominator - 1) /
                                    denominator);
        };
        setContentBounds(*scaled,
            ceilDivide(static_cast<int64_t>(source_right) * destination_width,
                       source->width),
            ceilDivide(static_cast<int64_t>(source_bottom) * destination_height,
                       source->height),
            ceilDivide(static_cast<int64_t>(source_left) * destination_width,
                       source->width),
            ceilDivide(static_cast<int64_t>(source_top) * destination_height,
                       source->height));
        if (source->black_bounds_valid) {
            setBlackBounds(*scaled,
                ceilDivide(static_cast<int64_t>(source->black_right) *
                               destination_width,
                           source->width),
                ceilDivide(static_cast<int64_t>(source->black_bottom) *
                               destination_height,
                           source->height),
                ceilDivide(static_cast<int64_t>(source->black_left) *
                               destination_width,
                           source->width),
                ceilDivide(static_cast<int64_t>(source->black_top) *
                               destination_height,
                           source->height));
        } else {
            scaled->black_bounds_valid = false;
        }
        return 1;
    }

    static int cImageDrawRotated(lua_State* state) {
        Impl* runtime = self(state);
        auto* image = static_cast<Image*>(luaL_checkudata(
            state, 1, kImageMetatable));
        const float x = static_cast<float>(luaL_checknumber(state, 2));
        const float y = static_cast<float>(luaL_checknumber(state, 3));
        const float angle = static_cast<float>(luaL_checknumber(state, 4));
        const float scale_x = static_cast<float>(luaL_optnumber(state, 5, 1.0));
        const float scale_y = static_cast<float>(luaL_optnumber(
            state, 6, scale_x));
        if (image) {
            runtime->drawImageRotated(*image, x, y, angle, scale_x, scale_y);
        }
        return 0;
    }

    static int cImageDrawFaded(lua_State* state) {
        Impl* runtime = self(state);
        auto* image = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        const int x = static_cast<int>(luaL_checknumber(state, 2));
        const int y = static_cast<int>(luaL_checknumber(state, 3));
        const float alpha = static_cast<float>(luaL_checknumber(state, 4));
        if (image) runtime->drawImage(*image, x, y, Unflipped, 1, alpha);
        return 0;
    }

    static int cImageSetMaskImage(lua_State* state) {
        auto* image = static_cast<Image*>(luaL_checkudata(
            state, 1, kImageMetatable));
        Image* mask = nullptr;
        if (!lua_isnoneornil(state, 2)) {
            mask = static_cast<Image*>(luaL_checkudata(
                state, 2, kImageMetatable));
            if (image && mask &&
                (image->width != mask->width || image->height != mask->height)) {
                return luaL_error(state,
                    "mask image must have the same dimensions as the image");
            }
        }
        if (image) image->mask = mask;
        if (mask) lua_pushvalue(state, 2);
        else lua_pushnil(state);
        lua_setiuservalue(state, 1, 1);
        return 0;
    }

    static int cImageGetMaskImage(lua_State* state) {
        luaL_checkudata(state, 1, kImageMetatable);
        lua_getiuservalue(state, 1, 1);
        return 1;
    }

    static int cImageGetSize(lua_State* state) {
        auto* image = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        lua_pushinteger(state, image ? image->width : 0);
        lua_pushinteger(state, image ? image->height : 0);
        return 2;
    }

    static int cImageSample(lua_State* state) {
        Impl* runtime = self(state);
        auto* image = static_cast<Image*>(luaL_checkudata(
            state, 1, kImageMetatable));
        const int x = static_cast<int>(luaL_checkinteger(state, 2));
        const int y = static_cast<int>(luaL_checkinteger(state, 3));
        uint8_t pixel = image ? runtime->imagePixel(*image, x, y)
                              : static_cast<uint8_t>(Clear);
        if (image && image->mask &&
            runtime->imagePixel(*image->mask, x, y) != White) {
            pixel = Clear;
        }
        lua_pushinteger(state, pixel == Black ? PdColorBlack :
            (pixel == White ? PdColorWhite : PdColorClear));
        return 1;
    }

    static int cImageAddMask(lua_State* state) {
        Impl* runtime = self(state);
        auto* image = static_cast<Image*>(luaL_checkudata(
            state, 1, kImageMetatable));
        if (!image || image->mask) return 0;
        const bool opaque = lua_isnoneornil(state, 2) ||
            lua_toboolean(state, 2) != 0;
        Image* mask = runtime->pushDynamicImage(
            image->width, image->height, opaque ? White : Black);
        if (!mask) return 0;
        image->mask = mask;
        lua_pushvalue(state, -1);
        lua_setiuservalue(state, 1, 1);
        return 0;
    }

    static int cImageRemoveMask(lua_State* state) {
        auto* image = static_cast<Image*>(luaL_checkudata(
            state, 1, kImageMetatable));
        if (image) image->mask = nullptr;
        lua_pushnil(state);
        lua_setiuservalue(state, 1, 1);
        return 0;
    }

    static int cImageHasMask(lua_State* state) {
        auto* image = static_cast<Image*>(luaL_checkudata(
            state, 1, kImageMetatable));
        lua_pushboolean(state, image && image->mask);
        return 1;
    }

    static int cImageClearMask(lua_State* state) {
        auto* image = static_cast<Image*>(luaL_checkudata(
            state, 1, kImageMetatable));
        const bool opaque = lua_isnoneornil(state, 2) ||
            lua_toboolean(state, 2) != 0;
        if (image && image->mask && image->mask->pixels) {
            std::memset(image->mask->pixels, opaque ? White : Black,
                static_cast<size_t>(image->mask->stride) * image->mask->height);
        }
        return 0;
    }

    static int cImageClear(lua_State* state) {
        auto* image = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        const uint8_t color = checkPlaydateColor(state, 2, Clear);
        if (image && image->pixels) {
            const size_t pixels = static_cast<size_t>(image->stride) * image->height;
            if (color == Xor) {
                for (size_t index = 0; index < pixels; ++index) {
                    uint8_t& value = image->pixels[index];
                    if (value == Black) value = White;
                    else if (value == White) value = Black;
                }
            } else {
                std::memset(image->pixels, color, pixels);
            }
            if (color != Xor) {
                setSolidImageBounds(*image, color);
            }
        }
        return 0;
    }

    static int cImageSetInverted(lua_State* state) {
        auto* image = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        if (image) image->inverted = lua_toboolean(state, 2) != 0;
        return 0;
    }

    static int cImageCopy(lua_State* state) {
        Impl* runtime = self(state);
        auto* source = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        if (!source) { lua_pushnil(state); return 1; }
        Image* copy = runtime->pushDynamicImage(source->width, source->height, Clear);
        if (!copy) return 1;
        for (int y = 0; y < source->height; ++y) for (int x = 0; x < source->width; ++x) {
            copy->pixels[y * copy->stride + x] = source->mask &&
                runtime->imagePixel(*source->mask, x, y) != White
                ? static_cast<uint8_t>(Clear)
                : runtime->imagePixel(*source, x, y);
        }
        int left = 0, top = 0, right = source->width, bottom = source->height;
        logicalContentBounds(*source, Unflipped, left, top, right, bottom);
        setContentBounds(*copy, right, bottom, left, top);
        if (source->black_bounds_valid) {
            setBlackBounds(*copy, source->black_right, source->black_bottom,
                           source->black_left, source->black_top);
        } else {
            copy->black_bounds_valid = false;
        }
        return 1;
    }

    static int cImageInverted(lua_State* state) {
        Impl* runtime = self(state);
        auto* source = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        if (!source) { lua_pushnil(state); return 1; }
        Image* copy = runtime->pushDynamicImage(source->width, source->height, Clear);
        if (!copy) return 1;
        for (int y = 0; y < source->height; ++y) for (int x = 0; x < source->width; ++x) {
            const uint8_t value = runtime->imagePixel(*source, x, y);
            copy->pixels[y * copy->stride + x] = source->mask &&
                runtime->imagePixel(*source->mask, x, y) != White
                ? Clear : (value == Black ? White :
                    (value == White ? Black : Clear));
        }
        int left = 0, top = 0, right = source->width, bottom = source->height;
        logicalContentBounds(*source, Unflipped, left, top, right, bottom);
        setContentBounds(*copy, right, bottom, left, top);
        copy->black_bounds_valid = false;
        return 1;
    }

    static int cImageFaded(lua_State* state) {
        Impl* runtime = self(state);
        auto* source = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        const float alpha = static_cast<float>(luaL_checknumber(state, 2));
        if (!source) { lua_pushnil(state); return 1; }
        Image* copy = runtime->pushDynamicImage(source->width, source->height, Clear);
        if (!copy) return 1;
        static constexpr uint8_t bayer[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
        const int threshold = std::clamp(static_cast<int>(alpha * 16.0f), 0, 16);
        for (int y = 0; y < source->height; ++y) for (int x = 0; x < source->width; ++x) {
            if (bayer[(y & 3) * 4 + (x & 3)] < threshold)
                copy->pixels[y * copy->stride + x] = source->mask &&
                    runtime->imagePixel(*source->mask, x, y) != White
                    ? static_cast<uint8_t>(Clear)
                    : runtime->imagePixel(*source, x, y);
        }
        int left = 0, top = 0, right = source->width, bottom = source->height;
        logicalContentBounds(*source, Unflipped, left, top, right, bottom);
        setContentBounds(*copy, right, bottom, left, top);
        if (source->black_bounds_valid) {
            setBlackBounds(*copy, source->black_right, source->black_bottom,
                           source->black_left, source->black_top);
        } else {
            copy->black_bounds_valid = false;
        }
        return 1;
    }

    static int cImageTableNew(lua_State* state) {
        Impl* runtime = self(state);
        const char* path = luaL_checkstring(state, 1);
        if (runtime->pushPdtTable(path)) return 1;
        const CelesteAsset* asset = findCelesteAsset(path);
        if (!asset || asset->frame_count <= 1) {
            lua_pushnil(state);
            return 1;
        }
        auto* table = static_cast<ImageTable*>(lua_newuserdatauv(
            state, sizeof(ImageTable), 1));
        new (table) ImageTable{asset};
        luaL_getmetatable(state, kImageTableMetatable);
        lua_setmetatable(state, -2);
        lua_newtable(state);
        lua_setiuservalue(state, -2, 1);
        return 1;
    }

    bool pushImageTableFrame(lua_State* state, int table_index,
                             ImageTable* table, int index) {
        table_index = lua_absindex(state, table_index);
        const int count = table ? (table->asset ? table->asset->frame_count : table->frame_count) : 0;
        if (!table || index < 1 || index > count) {
            return false;
        }
        lua_getiuservalue(state, table_index, 1);
        lua_rawgeti(state, -1, index);
        if (luaL_testudata(state, -1, kImageMetatable)) {
            lua_remove(state, -2);
            return true;
        }
        lua_pop(state, 1);
        Image* image = nullptr;
        if (table->asset) {
            image = pushImage();
            image->width = table->asset->frame_width;
            image->height = table->asset->frame_height;
            image->asset = table->asset;
            image->frame = index - 1;
        } else {
            const size_t table_bytes = 4U + static_cast<size_t>(table->frame_count) * 4U;
            const uint32_t previous = index > 1
                ? readLe32(table->data + 4U + static_cast<size_t>(index - 2) * 4U)
                : 0U;
            const uint32_t end = readLe32(table->data + 4U + static_cast<size_t>(index - 1) * 4U);
            if (table_bytes + end > table->data_size || previous >= end) {
                lua_pop(state, 1);
                return false;
            }
            image = pushDynamicImage(table->frame_width,
                                     table->frame_height, Clear);
            if (!image || !decodeSerializedImage(
                    *image, table->data + table_bytes + previous, end - previous,
                    table->frame_width, table->frame_height)) {
                if (image) lua_pop(state, 1);
                lua_pop(state, 1);
                return false;
            }
        }
        lua_pushvalue(state, -1);
        lua_rawseti(state, -3, index);
        lua_remove(state, -2);
        return true;
    }

    static int cImageTableGetImage(lua_State* state) {
        Impl* runtime = self(state);
        auto* table = static_cast<ImageTable*>(luaL_checkudata(
            state, 1, kImageTableMetatable));
        const int index = static_cast<int>(luaL_checkinteger(state, 2));
        if (!runtime || !runtime->pushImageTableFrame(state, 1, table, index)) {
            lua_pushnil(state);
        }
        return 1;
    }

    static int floorDivide(int value, int divisor) {
        int quotient = value / divisor;
        const int remainder = value % divisor;
        if (remainder < 0) --quotient;
        return quotient;
    }

    static int cDrawTilemap(lua_State* state) {
        Impl* runtime = self(state);
        auto* table = static_cast<ImageTable*>(luaL_checkudata(
            state, 1, kImageTableMetatable));
        luaL_checktype(state, 2, LUA_TTABLE);
        const int columns = std::max(1, static_cast<int>(
            luaL_checkinteger(state, 3)));
        const int origin_x = static_cast<int>(std::floor(
            luaL_optnumber(state, 4, 0.0)));
        const int origin_y = static_cast<int>(std::floor(
            luaL_optnumber(state, 5, 0.0)));
        if (!runtime || !table || !runtime->target) return 0;

        const int tile_width = table->asset
            ? table->asset->frame_width : table->frame_width;
        const int tile_height = table->asset
            ? table->asset->frame_height : table->frame_height;
        const int tile_count = static_cast<int>(lua_rawlen(state, 2));
        const int rows = (tile_count + columns - 1) / columns;
        if (tile_width <= 0 || tile_height <= 0 || rows <= 0) return 0;

        const int clip_left = std::max(0, runtime->clip.x);
        const int clip_top = std::max(0, runtime->clip.y);
        const int clip_right = std::min(runtime->target->width,
            runtime->clip.x + runtime->clip.w);
        const int clip_bottom = std::min(runtime->target->height,
            runtime->clip.y + runtime->clip.h);
        if (clip_left >= clip_right || clip_top >= clip_bottom) return 0;

        const int first_column = std::max(0,
            floorDivide(clip_left - origin_x, tile_width));
        const int last_column = std::min(columns - 1,
            floorDivide(clip_right - 1 - origin_x, tile_width));
        const int first_row = std::max(0,
            floorDivide(clip_top - origin_y, tile_height));
        const int last_row = std::min(rows - 1,
            floorDivide(clip_bottom - 1 - origin_y, tile_height));
        if (first_column > last_column || first_row > last_row) return 0;

        for (int row = first_row; row <= last_row; ++row) {
            for (int column = first_column; column <= last_column; ++column) {
                const int cell = row * columns + column + 1;
                if (cell > tile_count) break;
                lua_rawgeti(state, 2, cell);
                const int frame = lua_isnumber(state, -1)
                    ? static_cast<int>(lua_tointeger(state, -1)) : 0;
                lua_pop(state, 1);
                if (frame <= 0 || !runtime->pushImageTableFrame(
                        state, 1, table, frame)) {
                    continue;
                }
                auto* image = static_cast<Image*>(luaL_testudata(
                    state, -1, kImageMetatable));
                if (image) {
                    runtime->drawImage(*image,
                        origin_x + column * tile_width,
                        origin_y + row * tile_height, Unflipped);
                }
                lua_pop(state, 1);
            }
        }
        return 0;
    }

    static int cImageTableDrawImage(lua_State* state) {
        Impl* runtime = self(state);
        const int x = static_cast<int>(luaL_checknumber(state, 3));
        const int y = static_cast<int>(luaL_checknumber(state, 4));
        const int flip = static_cast<int>(luaL_optinteger(
            state, 5, Unflipped));
        cImageTableGetImage(state);
        auto* image = static_cast<Image*>(luaL_testudata(
            state, -1, kImageMetatable));
        if (image) runtime->drawImage(*image, x, y, flip);
        return 0;
    }

    static int cImageTableGetLength(lua_State* state) {
        auto* table = static_cast<ImageTable*>(luaL_checkudata(
            state, 1, kImageTableMetatable));
        const int count = table ? (table->asset
            ? table->asset->frame_count : table->frame_count) : 0;
        lua_pushinteger(state, count);
        return 1;
    }

    static int cImageTableGetSize(lua_State* state) {
        auto* table = static_cast<ImageTable*>(luaL_checkudata(
            state, 1, kImageTableMetatable));
        const int count = table ? (table->asset
            ? table->asset->frame_count : table->frame_count) : 0;
        // Sequential tables are the form supported by the current PDX
        // decoder.  Report their documented one-row cell layout.
        lua_pushinteger(state, count);
        lua_pushinteger(state, count > 0 ? 1 : 0);
        return 2;
    }

    static int cImageTableIndex(lua_State* state) {
        // Playdate image tables expose frames through both getImage(index) and
        // the Lua shorthand imageTable[index].  CoreLibs/animation uses the
        // latter, so a plain metatable-as-__index leaves every frame nil.
        if (lua_type(state, 2) == LUA_TNUMBER) {
            return cImageTableGetImage(state);
        }
        luaL_getmetatable(state, kImageTableMetatable);
        lua_pushvalue(state, 2);
        lua_rawget(state, -2);
        lua_remove(state, -2);
        return 1;
    }

    static int cImageTableLen(lua_State* state) {
        auto* table = static_cast<ImageTable*>(luaL_checkudata(state, 1, kImageTableMetatable));
        lua_pushinteger(state, table ? (table->asset ? table->asset->frame_count :
                                        table->frame_count) : 0);
        return 1;
    }

    bool readRect(lua_State* state, int start, int& x, int& y, int& w, int& h) {
        if (lua_istable(state, start)) {
            auto field = [&](const char* name, const char* fallback = nullptr) {
                lua_getfield(state, start, name);
                if (lua_isnil(state, -1) && fallback) {
                    lua_pop(state, 1);
                    lua_getfield(state, start, fallback);
                }
                const int value = static_cast<int>(lua_tonumber(state, -1));
                lua_pop(state, 1);
                return value;
            };
            x = field("x"); y = field("y");
            w = field("width", "w"); h = field("height", "h");
            return true;
        }
        x = static_cast<int>(luaL_checknumber(state, start));
        y = static_cast<int>(luaL_checknumber(state, start + 1));
        w = static_cast<int>(luaL_checknumber(state, start + 2));
        h = static_cast<int>(luaL_checknumber(state, start + 3));
        return true;
    }

    static int cGraphicsBeginFrame(lua_State* state) {
        self(state)->beginFrame();
        return 0;
    }

    static int cGetDisplayImage(lua_State* state) {
        Impl* runtime = self(state);
        if (!runtime->screen.pixels) {
            lua_pushnil(state);
            return 1;
        }
        Image* copy = runtime->pushDynamicImage(
            runtime->screen.width, runtime->screen.height, Clear);
        if (!copy) return 1;
        const size_t bytes = static_cast<size_t>(runtime->screen.width) *
            runtime->screen.height;
        std::memcpy(copy->pixels, runtime->screen.pixels, bytes);
        setContentBounds(*copy, copy->width, copy->height);
        copy->black_bounds_valid = false;
        return 1;
    }

    static int cGraphicsClear(lua_State* state) {
        Impl* runtime = self(state);
        const uint8_t color = checkPlaydateColor(
            state, 1, runtime->background_color);
        runtime->fillTarget(color);
        return 0;
    }

    static int cSetColor(lua_State* state) {
        Impl* runtime = self(state);
        runtime->solid_draw_color = checkPlaydateColor(state, 1, Black);
        runtime->draw_color = runtime->solid_draw_color;
        return 0;
    }

    static int cSetPattern(lua_State* state) {
        Impl* runtime = self(state);
        if (auto* image = static_cast<Image*>(
                luaL_testudata(state, 1, kImageMetatable))) {
            if (image->width <= 0 || image->height <= 0) {
                return luaL_error(state, "pattern image is empty");
            }
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    runtime->draw_pattern_pixels[static_cast<size_t>(y * 8 + x)] =
                        runtime->imagePixel(*image, x % image->width,
                                            y % image->height);
                }
            }
            runtime->draw_pattern_uses_image = true;
        } else {
            luaL_checktype(state, 1, LUA_TTABLE);
            for (lua_Integer row = 1; row <= 8; ++row) {
                lua_rawgeti(state, 1, row);
                const lua_Integer value = luaL_checkinteger(state, -1);
                lua_pop(state, 1);
                if (value < 0 || value > 255) {
                    return luaL_error(state,
                        "pattern row %d must be between 0 and 255",
                        static_cast<int>(row));
                }
                runtime->draw_pattern[static_cast<size_t>(row - 1)] =
                    static_cast<uint8_t>(value);
            }
            runtime->draw_pattern_uses_image = false;
        }
        runtime->pattern_offset_x = static_cast<int>(
            luaL_optinteger(state, 2, 0));
        runtime->pattern_offset_y = static_cast<int>(
            luaL_optinteger(state, 3, 0));
        runtime->draw_pattern_transparent = false;
        runtime->draw_color = Pattern;
        return 0;
    }

    static int cSetDitherPattern(lua_State* state) {
        Impl* runtime = self(state);
        const float alpha = std::clamp(
            static_cast<float>(luaL_checknumber(state, 1)), 0.0f, 1.0f);
        const int dither_type = static_cast<int>(
            luaL_optinteger(state, 2, 7)); // kDitherTypeBayer8x8

        static constexpr uint8_t bayer2[2][2] = {
            {0, 2}, {3, 1},
        };
        static constexpr uint8_t bayer4[4][4] = {
            {0, 8, 2, 10}, {12, 4, 14, 6},
            {3, 11, 1, 9}, {15, 7, 13, 5},
        };
        static constexpr uint8_t bayer8[8][8] = {
            {0,32,8,40,2,34,10,42}, {48,16,56,24,50,18,58,26},
            {12,44,4,36,14,46,6,38}, {60,28,52,20,62,30,54,22},
            {3,35,11,43,1,33,9,41}, {51,19,59,27,49,17,57,25},
            {15,47,7,39,13,45,5,37}, {63,31,55,23,61,29,53,21},
        };

        // Playdate's white dither pattern has its documented inverted-alpha
        // behavior. Black (the normal Noble use) maps 0 to transparent and 1
        // to opaque; white deliberately does the reverse.
        const bool white = runtime->solid_draw_color == White;
        const float density = white ? 1.0f - alpha : alpha;
        runtime->draw_pattern.fill(0U);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                int threshold = 0;
                int levels = 64;
                switch (dither_type) {
                    case 0: // none
                        threshold = 0; levels = 2; break;
                    case 1: // diagonal line
                        threshold = (x + y) & 7; levels = 8; break;
                    case 2: // vertical line
                        threshold = x; levels = 8; break;
                    case 3: // horizontal line
                        threshold = y; levels = 8; break;
                    case 4: // screen
                        threshold = ((x + y * 3) & 7); levels = 8; break;
                    case 5: // Bayer 2x2
                        threshold = bayer2[y & 1][x & 1]; levels = 4; break;
                    case 6: // Bayer 4x4
                        threshold = bayer4[y & 3][x & 3]; levels = 16; break;
                    default: // Bayer 8x8 and deterministic fallbacks
                        threshold = bayer8[y][x]; levels = 64; break;
                }
                const bool set = dither_type == 0
                    ? density >= 0.5f
                    : static_cast<float>(threshold) < density * levels;
                if (set) {
                    runtime->draw_pattern[static_cast<size_t>(y)] |=
                        static_cast<uint8_t>(0x80U >> x);
                }
            }
        }
        runtime->draw_pattern_color = white ? White : Black;
        runtime->draw_pattern_transparent = true;
        runtime->draw_pattern_uses_image = false;
        runtime->pattern_offset_x = 0;
        runtime->pattern_offset_y = 0;
        runtime->draw_color = Pattern;
        return 0;
    }

    static int cSetDrawMode(lua_State* state) {
        int mode = Copy;
        if (lua_type(state, 1) == LUA_TSTRING) {
            const char* value = lua_tostring(state, 1);
            if (std::strcmp(value, "copy") == 0) mode = Copy;
            else if (std::strcmp(value, "whiteTransparent") == 0)
                mode = WhiteTransparent;
            else if (std::strcmp(value, "blackTransparent") == 0)
                mode = BlackTransparent;
            else if (std::strcmp(value, "fillWhite") == 0) mode = FillWhite;
            else if (std::strcmp(value, "fillBlack") == 0) mode = FillBlack;
            else if (std::strcmp(value, "XOR") == 0) mode = DrawXor;
            else if (std::strcmp(value, "NXOR") == 0) mode = Nxor;
            else if (std::strcmp(value, "inverted") == 0) mode = Inverted;
            else return luaL_argerror(state, 1, "unknown image draw mode");
        } else {
            mode = static_cast<int>(luaL_checkinteger(state, 1));
            if (mode < Copy || mode > Inverted) {
                return luaL_argerror(state, 1, "invalid image draw mode");
            }
        }
        self(state)->draw_mode = mode;
        return 0;
    }

    static int cGetDrawMode(lua_State* state) {
        lua_pushinteger(state, self(state)->draw_mode);
        return 1;
    }

    static int cSetLineWidth(lua_State* state) {
        self(state)->line_width = std::clamp(static_cast<int>(luaL_checkinteger(state, 1)), 1, 4);
        return 0;
    }

    static int cSetStrokeLocation(lua_State* state) {
        self(state)->stroke_location = static_cast<int>(luaL_checkinteger(state, 1));
        return 0;
    }

    void fillRect(int x, int y, int width, int height, uint8_t color) {
        if (!target || !target->pixels || width <= 0 || height <= 0) return;
        const int left = std::max({x, clip.x, 0});
        const int top = std::max({y, clip.y, 0});
        const int right = std::min({x + width, clip.x + clip.w, target->width});
        const int bottom = std::min({y + height, clip.y + clip.h, target->height});
        if (left >= right || top >= bottom) return;

        // Background clears and layer rectangles account for most of
        // Celeste's filled pixels.  Clip once and write complete byte-pixel
        // spans instead of calling the generic compositor for every pixel
        // (the old 400x240 background call performed 96,000 function calls
        // even though the fullscreen logical target is only 200x120).
        if (!stencil && draw_mode != Nxor && color != Pattern) {
            uint8_t value = mappedColor(color, left, top);
            if (value == Clear && target == &screen) return;
            if (target == &screen && inverted_display && value != Clear) {
                value = value == Black ? White : Black;
            }
            const size_t span = static_cast<size_t>(right - left);
            for (int row = top; row < bottom; ++row) {
                std::memset(target->pixels +
                    static_cast<size_t>(row) * target->stride + left,
                    value, span);
            }
            if (target != &screen && value != Clear) {
                expandContentBounds(*target, left, top, right, bottom);
                if (value == Black) {
                    expandBlackBounds(*target, left, top, right, bottom);
                }
            }
            return;
        }

        for (int row = top; row < bottom; ++row) {
            for (int column = left; column < right; ++column) {
                putLogicalPixel(column, row, color);
            }
        }
    }

    static int cFillRect(lua_State* state) {
        Impl* runtime = self(state);
        int x, y, w, h; runtime->readRect(state, 1, x, y, w, h);
        runtime->fillRect(x, y, w, h, runtime->draw_color);
        return 0;
    }

    static int cDrawRect(lua_State* state) {
        Impl* runtime = self(state);
        int x, y, w, h; runtime->readRect(state, 1, x, y, w, h);
        for (int line = 0; line < runtime->line_width; ++line) {
            for (int px = x + line; px < x + w - line; ++px) {
                runtime->putLogicalPixel(px, y + line, runtime->draw_color);
                runtime->putLogicalPixel(px, y + h - 1 - line, runtime->draw_color);
            }
            for (int py = y + line; py < y + h - line; ++py) {
                runtime->putLogicalPixel(x + line, py, runtime->draw_color);
                runtime->putLogicalPixel(x + w - 1 - line, py, runtime->draw_color);
            }
        }
        return 0;
    }

    bool insideRoundedRect(int px, int py, int x, int y, int width,
                           int height, int radius) const {
        if (width <= 0 || height <= 0 || px < x || py < y ||
            px >= x + width || py >= y + height) return false;
        radius = std::clamp(radius, 0, std::min(width, height) / 2);
        if (radius == 0) return true;
        const int center_x = std::clamp(px, x + radius,
                                       x + width - 1 - radius);
        const int center_y = std::clamp(py, y + radius,
                                       y + height - 1 - radius);
        const int dx = px - center_x;
        const int dy = py - center_y;
        return dx * dx + dy * dy <= radius * radius;
    }

    void roundedRect(int x, int y, int width, int height, int radius,
                     bool fill) {
        const int inset = std::max(1, line_width);
        for (int py = y; py < y + height; ++py) {
            for (int px = x; px < x + width; ++px) {
                if (!insideRoundedRect(px, py, x, y, width, height, radius)) {
                    continue;
                }
                const bool inner = !fill && insideRoundedRect(
                    px, py, x + inset, y + inset,
                    width - inset * 2, height - inset * 2,
                    std::max(0, radius - inset));
                if (!inner) putLogicalPixel(px, py, draw_color);
            }
        }
    }

    static int cFillRoundRect(lua_State* state) {
        Impl* runtime = self(state);
        int x, y, width, height;
        runtime->readRect(state, 1, x, y, width, height);
        const int radius_index = lua_istable(state, 1) ? 2 : 5;
        const int radius = static_cast<int>(
            luaL_checkinteger(state, radius_index));
        runtime->roundedRect(x, y, width, height, radius, true);
        return 0;
    }

    static int cDrawRoundRect(lua_State* state) {
        Impl* runtime = self(state);
        int x, y, width, height;
        runtime->readRect(state, 1, x, y, width, height);
        const int radius_index = lua_istable(state, 1) ? 2 : 5;
        const int radius = static_cast<int>(
            luaL_checkinteger(state, radius_index));
        runtime->roundedRect(x, y, width, height, radius, false);
        return 0;
    }

    static int cDrawLine(lua_State* state) {
        Impl* runtime = self(state);
        int x0 = static_cast<int>(luaL_checknumber(state, 1));
        int y0 = static_cast<int>(luaL_checknumber(state, 2));
        const int x1 = static_cast<int>(luaL_checknumber(state, 3));
        const int y1 = static_cast<int>(luaL_checknumber(state, 4));
        const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int error = dx + dy;
        while (true) {
            runtime->putLogicalPixel(x0, y0, runtime->draw_color);
            if (x0 == x1 && y0 == y1) break;
            const int twice = error * 2;
            if (twice >= dy) { error += dy; x0 += sx; }
            if (twice <= dx) { error += dx; y0 += sy; }
        }
        return 0;
    }

    void circle(int cx, int cy, int radius, bool fill) {
        const int rr = radius * radius;
        const int inner = std::max(0, radius - line_width);
        const int ii = inner * inner;
        for (int y = -radius; y <= radius; ++y) for (int x = -radius; x <= radius; ++x) {
            const int distance = x*x + y*y;
            if (distance <= rr && (fill || distance >= ii))
                putLogicalPixel(cx + x, cy + y, draw_color);
        }
    }

    static int cFillCircle(lua_State* state) {
        Impl* runtime = self(state);
        runtime->circle(static_cast<int>(luaL_checknumber(state, 1)),
                        static_cast<int>(luaL_checknumber(state, 2)),
                        static_cast<int>(luaL_checknumber(state, 3)), true);
        return 0;
    }

    static int cDrawCircle(lua_State* state) {
        Impl* runtime = self(state);
        runtime->circle(static_cast<int>(luaL_checknumber(state, 1)),
                        static_cast<int>(luaL_checknumber(state, 2)),
                        static_cast<int>(luaL_checknumber(state, 3)), false);
        return 0;
    }

    static int cFillCircleInRect(lua_State* state) {
        Impl* runtime = self(state);
        int x, y, w, h; runtime->readRect(state, 1, x, y, w, h);
        runtime->circle(x + w/2, y + h/2, std::max(0, std::min(w,h)/2), true);
        return 0;
    }

    static int cDrawCircleInRect(lua_State* state) {
        Impl* runtime = self(state);
        int x, y, w, h; runtime->readRect(state, 1, x, y, w, h);
        runtime->circle(x + w/2, y + h/2, std::max(0, std::min(w,h)/2), false);
        return 0;
    }

    int picoFrame(unsigned char character) const {
        if (character >= '0' && character <= '9') return character - '0';
        if (character == ' ') return 10;
        if (character >= 'A' && character <= 'Z') return 12 + character - 'A';
        static constexpr char punctuation[] = ":;!=-+~@#$%^?._][()&'*,/<>\\`|{}";
        const char* found = std::strchr(punctuation, character);
        if (found) return 38 + static_cast<int>(found - punctuation);
        if (character >= 'a' && character <= 'z') return 69 + character - 'a';
        return 11;
    }

    struct CompiledGlyph {
        const uint8_t* cell = nullptr;
        size_t cell_size = 0;
        int advance = 0;
    };

    static unsigned bitCount(uint8_t value) {
        unsigned count = 0;
        while (value) {
            value = static_cast<uint8_t>(value & (value - 1U));
            ++count;
        }
        return count;
    }

    static size_t flagCount(const uint8_t* flags, size_t bytes) {
        size_t count = 0;
        for (size_t index = 0; index < bytes; ++index) {
            count += bitCount(flags[index]);
        }
        return count;
    }

    static size_t flagOrdinal(const uint8_t* flags, unsigned bit_index) {
        size_t ordinal = flagCount(flags, bit_index >> 3U);
        const unsigned bit = bit_index & 7U;
        if (bit != 0U) {
            ordinal += bitCount(static_cast<uint8_t>(
                flags[bit_index >> 3U] & ((1U << bit) - 1U)));
        }
        return ordinal;
    }

    static uint32_t nextUtf8(const char* text, size_t length, size_t& offset) {
        if (!text || offset >= length) return 0;
        const uint8_t first = static_cast<uint8_t>(text[offset++]);
        if (first < 0x80U) return first;
        unsigned continuation_count = 0;
        uint32_t codepoint = 0;
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1; codepoint = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2; codepoint = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3; codepoint = first & 0x07U;
        } else {
            return 0xfffdU;
        }
        if (continuation_count > length - offset) {
            offset = length;
            return 0xfffdU;
        }
        for (unsigned index = 0; index < continuation_count; ++index) {
            const uint8_t value = static_cast<uint8_t>(text[offset]);
            if ((value & 0xc0U) != 0x80U) return 0xfffdU;
            ++offset;
            codepoint = (codepoint << 6U) | (value & 0x3fU);
        }
        return codepoint <= 0x3ffffU ? codepoint : 0xfffdU;
    }

    bool compiledGlyph(const PdFont& font, uint32_t codepoint,
                       uint32_t next_codepoint, CompiledGlyph& result) const {
        result = {};
        if (!font.compiled || font.data_size < 68U || codepoint > 0x1ffffU) {
            return false;
        }
        const uint8_t* data = font.data();
        const uint8_t* page_flags = data + 4U;
        const unsigned page_index = static_cast<unsigned>(codepoint >> 8U);
        if ((page_flags[page_index >> 3U] &
             (1U << (page_index & 7U))) == 0U) {
            return false;
        }

        const size_t page_count = flagCount(page_flags, 64U);
        const size_t page_ordinal = flagOrdinal(page_flags, page_index);
        const size_t page_table_size = page_count * 4U;
        if (page_count == 0U || page_table_size > font.data_size - 68U) {
            return false;
        }
        const uint8_t* page_offsets = data + 68U;
        const uint8_t* pages = page_offsets + page_table_size;
        const size_t pages_size = font.data_size - 68U - page_table_size;
        const uint32_t page_start = page_ordinal == 0U ? 0U
            : readLe32(page_offsets + (page_ordinal - 1U) * 4U);
        const uint32_t page_end = readLe32(
            page_offsets + page_ordinal * 4U);
        if (page_start > page_end || page_end > pages_size ||
            page_end - page_start < 36U) {
            return false;
        }

        const uint8_t* page = pages + page_start;
        const size_t page_size = page_end - page_start;
        const uint8_t* glyph_flags = page + 4U;
        const unsigned glyph_index = static_cast<unsigned>(codepoint & 0xffU);
        if ((glyph_flags[glyph_index >> 3U] &
             (1U << (glyph_index & 7U))) == 0U) {
            return false;
        }
        const size_t glyph_count = flagCount(glyph_flags, 32U);
        const size_t glyph_ordinal = flagOrdinal(glyph_flags, glyph_index);
        const size_t glyph_table_size = glyph_count * 2U;
        if (glyph_count == 0U || glyph_table_size > page_size - 36U) {
            return false;
        }
        const uint8_t* glyph_offsets = page + 36U;
        const uint8_t* glyphs = glyph_offsets + glyph_table_size;
        const size_t glyphs_size = page_size - 36U - glyph_table_size;
        const uint16_t glyph_start = glyph_ordinal == 0U ? 0U
            : readLe16(glyph_offsets + (glyph_ordinal - 1U) * 2U);
        const uint16_t glyph_end = readLe16(
            glyph_offsets + glyph_ordinal * 2U);
        if (glyph_start > glyph_end || glyph_end > glyphs_size ||
            static_cast<size_t>(glyph_end - glyph_start) < 4U) {
            return false;
        }

        const uint8_t* glyph = glyphs + glyph_start;
        size_t glyph_size = glyph_end - glyph_start;

        // pdc aligns each glyph header to a four-byte boundary in the whole
        // unpacked PFT, not relative to the beginning of the glyph blob. A
        // page whose offset table ends at address 2 mod 4 therefore places two
        // zero padding bytes before every header. Treating those bytes as the
        // advance/kerning header made valid fonts (including Duel Of Shadows)
        // resolve to an empty or malformed image cell.
        const size_t glyph_data_offset = static_cast<size_t>(glyph - data);
        const size_t header_padding =
            (4U - (glyph_data_offset & 3U)) & 3U;
        if (header_padding > glyph_size ||
            glyph_size - header_padding < 4U) {
            return false;
        }
        glyph += header_padding;
        glyph_size -= header_padding;
        const size_t short_count = glyph[1];
        const size_t long_count = readLe16(glyph + 2U);
        const size_t short_end = 4U + short_count * 2U;
        const size_t long_start = (short_end + 3U) & ~size_t{3U};
        const size_t cell_start = long_start + long_count * 4U;
        if (short_end > glyph_size || long_start > glyph_size ||
            cell_start > glyph_size) {
            return false;
        }

        int kerning = 0;
        if ((next_codepoint >> 8U) == page_index) {
            for (size_t index = 0; index < short_count; ++index) {
                const uint8_t* entry = glyph + 4U + index * 2U;
                if (entry[0] == (next_codepoint & 0xffU)) {
                    kerning = static_cast<int8_t>(entry[1]);
                    break;
                }
            }
        }
        if (kerning == 0) {
            for (size_t index = 0; index < long_count; ++index) {
                const uint8_t* entry = glyph + long_start + index * 4U;
                const uint32_t other = static_cast<uint32_t>(entry[0]) |
                    (static_cast<uint32_t>(entry[1]) << 8U) |
                    (static_cast<uint32_t>(entry[2]) << 16U);
                if (other == next_codepoint) {
                    kerning = static_cast<int8_t>(entry[3]);
                    break;
                }
            }
        }
        result.cell = glyph + cell_start;
        result.cell_size = glyph_size - cell_start;
        result.advance = static_cast<int>(glyph[0]) + font.tracking + kerning;
        return true;
    }

    void drawCompiledGlyph(const PdFont& font, const CompiledGlyph& glyph,
                           int x, int y) {
        if (!glyph.cell || glyph.cell_size < 16U) return;
        const uint8_t* cell = glyph.cell;
        const uint16_t stored_width = readLe16(cell);
        const uint16_t stored_height = readLe16(cell + 2U);
        const uint16_t row_bytes = readLe16(cell + 4U);
        const uint16_t left = readLe16(cell + 6U);
        const uint16_t top = readLe16(cell + 10U);
        const uint16_t flags = readLe16(cell + 14U);
        if (stored_width == 0U || stored_height == 0U) return;
        const size_t plane_size = static_cast<size_t>(row_bytes) * stored_height;
        const bool has_mask = (flags & 0x03U) != 0U;
        const size_t plane_count = has_mask ? 2U : 1U;
        if (row_bytes == 0U || stored_width > row_bytes * 8U ||
            left + stored_width > font.glyph_width ||
            top + stored_height > font.glyph_height ||
            plane_size > (glyph.cell_size - 16U) / plane_count) {
            return;
        }
        const uint8_t* bitmap = cell + 16U;
        const uint8_t* mask = has_mask ? bitmap + plane_size : nullptr;
        for (uint16_t row = 0; row < stored_height; ++row) {
            for (uint16_t column = 0; column < stored_width; ++column) {
                const uint8_t bit = static_cast<uint8_t>(
                    0x80U >> (column & 7U));
                const size_t index = static_cast<size_t>(row) * row_bytes +
                    (column >> 3U);
                if (mask && (mask[index] & bit) == 0U) continue;
                putLogicalPixel(x + left + column, y + top + row,
                    (bitmap[index] & bit) != 0U ? White : Black);
            }
        }
    }

    static bool playdateButtonSymbol(uint32_t codepoint, char& label) {
        if (codepoint == 0x24b6U) {
            label = 'A';
            return true;
        }
        if (codepoint == 0x24b7U) {
            label = 'B';
            return true;
        }
        return false;
    }

    static int buttonSymbolAdvance(int scale = 1) {
        return 8 * std::max(1, scale);
    }

    void drawButtonSymbol(uint32_t codepoint, int x, int y, int scale = 1) {
        char label = 0;
        if (!playdateButtonSymbol(codepoint, label)) return;
        scale = std::max(1, scale);
        static constexpr uint8_t ring_rows[7] = {
            0x1cU, 0x22U, 0x41U, 0x41U, 0x41U, 0x22U, 0x1cU,
        };
        static constexpr uint8_t letter_a[5] = {
            0x2U, 0x5U, 0x7U, 0x5U, 0x5U,
        };
        static constexpr uint8_t letter_b[5] = {
            0x6U, 0x5U, 0x6U, 0x5U, 0x6U,
        };
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 7; ++column) {
                if ((ring_rows[row] & (1U << column)) == 0U) continue;
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        putLogicalPixel(x + column * scale + sx,
                            y + row * scale + sy, Black);
                    }
                }
            }
        }
        const uint8_t* rows = label == 'A' ? letter_a : letter_b;
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 3; ++column) {
                if ((rows[row] & (1U << (2 - column))) == 0U) continue;
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        putLogicalPixel(x + (column + 2) * scale + sx,
                            y + (row + 1) * scale + sy, Black);
                    }
                }
            }
        }
    }

    int textWidthForFont(const PdFont* font, const char* text,
                         size_t length) const {
        if (!text) return 0;
        int line_width = 0;
        int maximum_width = 0;
        size_t offset = 0;
        while (offset < length) {
            const uint32_t codepoint = nextUtf8(text, length, offset);
            if (codepoint == '\n') {
                maximum_width = std::max(maximum_width, line_width);
                line_width = 0;
                continue;
            }
            size_t lookahead = offset;
            const uint32_t next_codepoint = lookahead < length
                ? nextUtf8(text, length, lookahead) : 0U;
            if (font && font->compiled) {
                CompiledGlyph glyph{};
                if (compiledGlyph(*font, codepoint, next_codepoint, glyph)) {
                    line_width += glyph.advance > 0 ? glyph.advance
                        : std::max<int>(1, font->glyph_width);
                } else {
                    char button_label = 0;
                    if (playdateButtonSymbol(codepoint, button_label)) {
                        line_width += buttonSymbolAdvance();
                    } else {
                        if (codepoint != '?') {
                            compiledGlyph(*font, '?', next_codepoint, glyph);
                        }
                        line_width += glyph.advance > 0 ? glyph.advance
                            : std::max<int>(1, font->glyph_width);
                    }
                }
            } else if (font && font->pico) {
                char button_label = 0;
                line_width += playdateButtonSymbol(codepoint, button_label)
                    ? buttonSymbolAdvance() : 4;
            } else {
                char button_label = 0;
                const int scale = font ? font->scale : 1;
                line_width += playdateButtonSymbol(codepoint, button_label)
                    ? buttonSymbolAdvance(scale) : 6 * scale;
            }
        }
        return std::max(maximum_width, line_width);
    }

    int textWidth(const char* text, size_t length) const {
        return textWidthForFont(current_font, text, length);
    }

    int textHeightForFont(const PdFont* font, const char* text,
                          size_t length) const {
        int lines = 1;
        for (size_t index = 0; index < length; ++index) {
            if (text[index] == '\n') ++lines;
        }
        const int line_height = font && font->compiled
            ? std::max<int>(1, font->glyph_height)
            : (font && font->pico ? 5 : 7 * (font ? font->scale : 1));
        return text && length > 0 ? lines * line_height : 0;
    }

    static int cGetTextSize(lua_State* state) {
        Impl* runtime = self(state);
        size_t length = 0;
        const char* text = luaL_tolstring(state, 1, &length);
        const PdFont* font = runtime->current_font;
        if (lua_gettop(state) >= 3 && luaL_testudata(
                state, 2, kFontMetatable)) {
            font = static_cast<PdFont*>(lua_touserdata(state, 2));
        }
        const int width = runtime->textWidthForFont(font, text, length);
        const int height = runtime->textHeightForFont(font, text, length);
        lua_pop(state, 1);  // luaL_tolstring result
        lua_pushinteger(state, width);
        lua_pushinteger(state, height);
        return 2;
    }

    void drawText(const char* text, size_t length, int x, int y) {
        if (!text) return;
        int cursor_x = x, cursor_y = y;
        const bool pico = current_font && current_font->pico;
        const int scale = current_font ? current_font->scale : 1;
        const CelesteAsset* font_asset = pico ? findCelesteAsset("Assets/pico") : nullptr;
        size_t offset = 0;
        while (offset < length) {
            const uint32_t codepoint = nextUtf8(text, length, offset);
            if (codepoint == '\n') {
                cursor_x = x;
                cursor_y += current_font && current_font->compiled
                    ? std::max<int>(1, current_font->glyph_height)
                    : (pico ? 6 : 8 * scale);
                continue;
            }
            size_t lookahead = offset;
            const uint32_t next_codepoint = lookahead < length
                ? nextUtf8(text, length, lookahead) : 0U;
            if (current_font && current_font->compiled) {
                CompiledGlyph glyph{};
                if (compiledGlyph(*current_font, codepoint, next_codepoint,
                                  glyph)) {
                    drawCompiledGlyph(*current_font, glyph, cursor_x, cursor_y);
                    cursor_x += glyph.advance > 0 ? glyph.advance
                        : std::max<int>(1, current_font->glyph_width);
                    continue;
                }
                char button_label = 0;
                if (playdateButtonSymbol(codepoint, button_label)) {
                    drawButtonSymbol(codepoint, cursor_x, cursor_y);
                    cursor_x += buttonSymbolAdvance();
                    continue;
                }
                if (codepoint != '?') {
                    compiledGlyph(*current_font, '?', next_codepoint, glyph);
                }
                drawCompiledGlyph(*current_font, glyph, cursor_x, cursor_y);
                cursor_x += glyph.advance > 0 ? glyph.advance
                    : std::max<int>(1, current_font->glyph_width);
                continue;
            }
            char button_label = 0;
            if (playdateButtonSymbol(codepoint, button_label)) {
                drawButtonSymbol(codepoint, cursor_x, cursor_y, pico ? 1 : scale);
                cursor_x += buttonSymbolAdvance(pico ? 1 : scale);
                continue;
            }
            const unsigned char character = codepoint <= 0xffU
                ? static_cast<unsigned char>(codepoint) : '?';
            if (pico && font_asset) {
                Image glyph{};
                glyph.width = 3; glyph.height = 5; glyph.asset = font_asset;
                glyph.frame = picoFrame(character);
                drawImage(glyph, cursor_x, cursor_y, Unflipped);
                cursor_x += 4;
            } else {
                const gfx::Font& font = gfx::font5x7();
                for (int column = 0; column < 5; ++column) {
                    const uint8_t bits = font.glyph_column(static_cast<char>(character), column);
                    for (int row = 0; row < 7; ++row) if ((bits >> row) & 1U) {
                        for (int sy = 0; sy < scale; ++sy) for (int sx = 0; sx < scale; ++sx)
                            putLogicalPixel(cursor_x + column*scale + sx,
                                            cursor_y + row*scale + sy, Black);
                    }
                }
                cursor_x += 6 * scale;
            }
        }
    }

    static int cDrawText(lua_State* state) {
        Impl* runtime = self(state);
        size_t length = 0;
        const char* value = luaL_tolstring(state, 1, &length);
        const int x = static_cast<int>(luaL_checknumber(state, 2));
        const int y = static_cast<int>(luaL_checknumber(state, 3));
        runtime->drawText(value, length, x, y);
        lua_pop(state, 1);
        return 0;
    }

    static int cDrawTextInRect(lua_State* state) {
        Impl* runtime = self(state);
        const int argument_count = lua_gettop(state);
        const int alignment = argument_count >= 8 && lua_isnumber(state, 8)
            ? static_cast<int>(lua_tointeger(state, 8)) : 0;
        size_t length = 0;
        const char* value = luaL_tolstring(state, 1, &length);
        int x, y, w, h; runtime->readRect(state, 2, x, y, w, h);
        int draw_x = x;
        const int width = runtime->textWidth(value, length);
        if (alignment == 1) draw_x = x + std::max(0, (w - width) / 2);
        else if (alignment == 2) draw_x = x + std::max(0, w - width);
        runtime->drawText(value, length, draw_x, y);
        lua_pop(state, 1);
        return 0;
    }

    static int cDrawTextAligned(lua_State* state) {
        Impl* runtime = self(state);
        size_t length = 0;
        const char* value = luaL_tolstring(state, 1, &length);
        int x = static_cast<int>(std::lround(luaL_checknumber(state, 2)));
        const int y = static_cast<int>(std::lround(luaL_checknumber(state, 3)));
        const int alignment = static_cast<int>(luaL_optinteger(state, 4, 0));
        const int width = runtime->textWidth(value, length);
        if (alignment == 1) x -= width / 2;
        else if (alignment == 2) x -= width;
        runtime->drawText(value, length, x, y);
        lua_pop(state, 1);
        return 0;
    }

    static int cFontNew(lua_State* state) {
        Impl* runtime = self(state);
        const char* path = luaL_optstring(state, 1, "");
        uint8_t* bytes = nullptr;
        size_t size = 0;
        uint32_t maximum_width = 0, maximum_height = 0, unused = 0;
        if (runtime->loadCompiledResource(path, ".pft", "Playdate FNT",
                                          bytes, size, maximum_width,
                                          maximum_height, unused) &&
            bytes && size >= 68U && bytes[0] > 0U && bytes[1] > 0U &&
            maximum_width <= 255U && maximum_height <= 255U) {
            auto* font = static_cast<PdFont*>(lua_newuserdatauv(
                state, sizeof(PdFont) + size, 0));
            new (font) PdFont{};
            font->compiled = true;
            font->tracking = static_cast<int16_t>(readLe16(bytes + 2U));
            font->glyph_width = bytes[0];
            font->glyph_height = bytes[1];
            font->data_size = static_cast<uint32_t>(size);
            std::memcpy(reinterpret_cast<uint8_t*>(font + 1), bytes, size);
            heap_caps_free(bytes);
            luaL_getmetatable(state, kFontMetatable);
            lua_setmetatable(state, -2);
            ESP_LOGI(TAG, "PFT ready: %s (%ux%u, %u bytes)", path,
                     static_cast<unsigned>(font->glyph_width),
                     static_cast<unsigned>(font->glyph_height),
                     static_cast<unsigned>(font->data_size));
            return 1;
        }
        if (bytes) heap_caps_free(bytes);
        auto* font = static_cast<PdFont*>(lua_newuserdatauv(
            state, sizeof(PdFont), 0));
        new (font) PdFont{};
        font->pico = path && std::strstr(path, "Assets/pico");
        font->scale = (!font->pico && path && std::strstr(path, "-20-")) ? 2 : 1;
        luaL_getmetatable(state, kFontMetatable);
        lua_setmetatable(state, -2);
        return 1;
    }

    static int cFontGetHeight(lua_State* state) {
        auto* font = static_cast<PdFont*>(luaL_checkudata(state, 1, kFontMetatable));
        lua_pushinteger(state, font && font->compiled ? font->glyph_height :
            (font && font->pico ? 5 : 7 * (font ? font->scale : 1)));
        return 1;
    }

    static int cFontGetTextWidth(lua_State* state) {
        Impl* runtime = self(state);
        auto* font = static_cast<PdFont*>(
            luaL_checkudata(state, 1, kFontMetatable));
        size_t length = 0;
        const char* text = luaL_checklstring(state, 2, &length);
        lua_pushinteger(state, runtime->textWidthForFont(font, text, length));
        return 1;
    }

    int pushSystemFont() {
        if (system_font_ref != LUA_NOREF) {
            lua_rawgeti(lua, LUA_REGISTRYINDEX, system_font_ref);
            return 1;
        }
        auto* font = static_cast<PdFont*>(lua_newuserdatauv(
            lua, sizeof(PdFont), 0));
        new (font) PdFont{};
        font->scale = 1;
        luaL_getmetatable(lua, kFontMetatable);
        lua_setmetatable(lua, -2);
        lua_pushvalue(lua, -1);
        system_font_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
        return 1;
    }

    static int cGetSystemFont(lua_State* state) {
        // Pogopo has one built-in 5x7 system face. The optional normal/bold/
        // italic variant is accepted; all variants resolve to that same face.
        return self(state)->pushSystemFont();
    }

    static int cGetFont(lua_State* state) {
        Impl* runtime = self(state);
        if (runtime->current_font_ref != LUA_NOREF) {
            lua_rawgeti(state, LUA_REGISTRYINDEX, runtime->current_font_ref);
            return 1;
        }
        return runtime->pushSystemFont();
    }

    static int cSetFont(lua_State* state) {
        Impl* runtime = self(state);
        runtime->current_font = static_cast<PdFont*>(
            luaL_checkudata(state, 1, kFontMetatable));
        if (runtime->current_font_ref != LUA_NOREF) {
            luaL_unref(state, LUA_REGISTRYINDEX, runtime->current_font_ref);
        }
        lua_pushvalue(state, 1);
        runtime->current_font_ref = luaL_ref(state, LUA_REGISTRYINDEX);
        return 0;
    }

    static int cSetFontFamily(lua_State* state) {
        Impl* runtime = self(state);
        if (auto* direct = static_cast<PdFont*>(
                luaL_testudata(state, 1, kFontMetatable))) {
            runtime->current_font = direct;
            if (runtime->current_font_ref != LUA_NOREF) {
                luaL_unref(state, LUA_REGISTRYINDEX, runtime->current_font_ref);
            }
            lua_pushvalue(state, 1);
            runtime->current_font_ref = luaL_ref(state, LUA_REGISTRYINDEX);
            return 0;
        }
        luaL_checktype(state, 1, LUA_TTABLE);

        // SDK font families use the variant keys "normal", "bold" and
        // "italic".  Old SDK output is also seen with a zero-based numeric
        // normal slot, so accept both without tying compatibility to one SDK.
        lua_getfield(state, 1, "normal");
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            lua_rawgeti(state, 1, 0);
        }
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            lua_rawgeti(state, 1, 1);
        }
        auto* font = static_cast<PdFont*>(
            luaL_testudata(state, -1, kFontMetatable));
        if (!font) {
            lua_pop(state, 1);
            // Missing variants are valid according to the SDK.  If normal is
            // absent, choose the first actual font in the family.
            lua_pushnil(state);
            while (lua_next(state, 1) != 0) {
                font = static_cast<PdFont*>(
                    luaL_testudata(state, -1, kFontMetatable));
                if (font) {
                    lua_remove(state, -2); // discard iterator key
                    break;
                }
                lua_pop(state, 1);
            }
        }
        if (!font) return 0;
        runtime->current_font = font;
        if (runtime->current_font_ref != LUA_NOREF) {
            luaL_unref(state, LUA_REGISTRYINDEX, runtime->current_font_ref);
        }
        runtime->current_font_ref = luaL_ref(state, LUA_REGISTRYINDEX);
        return 0;
    }

    static int cPushContext(lua_State* state) {
        Impl* runtime = self(state);
        auto* image = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        if (!image || !image->pixels || runtime->context_depth >= runtime->context_stack.size()) return 0;
        runtime->context_stack[runtime->context_depth++] =
            {runtime->target, runtime->clip, runtime->stencil};
        runtime->target = image;
        runtime->clip = {0, 0, image->width, image->height};
        runtime->stencil = nullptr;
        return 0;
    }

    static int cPopContext(lua_State* state) {
        Impl* runtime = self(state);
        if (runtime->context_depth == 0) return 0;
        const Context restored = runtime->context_stack[--runtime->context_depth];
        runtime->target = restored.target;
        runtime->clip = restored.clip;
        runtime->stencil = restored.stencil;
        return 0;
    }

    static int cLockFocus(lua_State* state) {
        Impl* runtime = self(state);
        auto* image = static_cast<Image*>(
            luaL_checkudata(state, 1, kImageMetatable));
        if (!image || !image->pixels) return 0;
        runtime->target = image;
        runtime->clip = {0, 0, image->width, image->height};
        runtime->stencil = nullptr;
        return 0;
    }

    static int cUnlockFocus(lua_State* state) {
        Impl* runtime = self(state);
        runtime->target = &runtime->screen;
        runtime->clip = {0, 0, runtime->screen.width, runtime->screen.height};
        runtime->stencil = nullptr;
        return 0;
    }

    static int cSetClipRect(lua_State* state) {
        Impl* runtime = self(state);
        int x, y, w, h; runtime->readRect(state, 1, x, y, w, h);
        runtime->clip = {x, y, w, h};
        return 0;
    }

    static int cGetClipRect(lua_State* state) {
        const ClipRect& clip = self(state)->clip;
        lua_pushinteger(state, clip.x);
        lua_pushinteger(state, clip.y);
        lua_pushinteger(state, clip.w);
        lua_pushinteger(state, clip.h);
        return 4;
    }

    static int cClearClipRect(lua_State* state) {
        Impl* runtime = self(state);
        runtime->clip = {0, 0, runtime->target->width, runtime->target->height};
        return 0;
    }

    static int cSetStencil(lua_State* state) {
        self(state)->stencil = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        return 0;
    }

    static int cClearStencil(lua_State* state) {
        self(state)->stencil = nullptr;
        return 0;
    }

    static int cSetBackgroundColor(lua_State* state) {
        Impl* runtime = self(state);
        const uint8_t color = checkPlaydateColor(state, 1, White);
        if (color != Black && color != White) {
            return luaL_argerror(state, 1,
                "background color must be kColorBlack or kColorWhite");
        }
        runtime->background_color = color;
        return 0;
    }

    static int cDisplayWidth(lua_State* state) {
        lua_pushinteger(state, 400 / self(state)->display_scale);
        return 1;
    }

    static int cDisplayHeight(lua_State* state) {
        lua_pushinteger(state, 240 / self(state)->display_scale);
        return 1;
    }

    static int cSetRefreshRate(lua_State* state) {
        Impl* runtime = self(state);
        runtime->refresh_rate = static_cast<uint32_t>(std::clamp(
            static_cast<int>(luaL_checkinteger(state, 1)), 1,
            static_cast<int>(kMaximumFps)));
        runtime->runtime_stats.requested_fps = runtime->refresh_rate;
        runtime->frame_accumulator_units = 0;
        lua_pushinteger(state, static_cast<lua_Integer>(runtime->refresh_rate));
        return 1;
    }

    static int cGetRefreshRate(lua_State* state) {
        lua_pushinteger(state, static_cast<lua_Integer>(self(state)->refresh_rate));
        return 1;
    }

    static int cSetScale(lua_State* state) {
        Impl* runtime = self(state);
        const int scale = std::clamp(
            static_cast<int>(luaL_checkinteger(state, 1)), 1, 2);
        if (!runtime->resizeScreen(scale)) {
            return luaL_error(state, "screen resize failed for scale %d", scale);
        }
        runtime->display_scale = scale;
        runtime->resetTargetToScreen();
        return 0;
    }

    static int cSetOffset(lua_State* state) {
        Impl* runtime = self(state);
        runtime->display_offset_x = static_cast<int>(std::lround(luaL_checknumber(state, 1)));
        runtime->display_offset_y = static_cast<int>(std::lround(luaL_checknumber(state, 2)));
        return 0;
    }

    static int cSetDrawOffset(lua_State* state) {
        Impl* runtime = self(state);
        runtime->draw_offset_x = static_cast<int>(std::lround(
            luaL_checknumber(state, 1)));
        runtime->draw_offset_y = static_cast<int>(std::lround(
            luaL_checknumber(state, 2)));
        return 0;
    }

    static int cGetDrawOffset(lua_State* state) {
        Impl* runtime = self(state);
        lua_pushinteger(state, runtime->draw_offset_x);
        lua_pushinteger(state, runtime->draw_offset_y);
        return 2;
    }

    static int cSetInverted(lua_State* state) {
        self(state)->inverted_display = lua_toboolean(state, 1) != 0;
        return 0;
    }

    static int cStartAccelerometer(lua_State* state) {
        self(state)->accelerometer_running = true;
        return 0;
    }

    static int cStopAccelerometer(lua_State* state) {
        self(state)->accelerometer_running = false;
        return 0;
    }

    static int cAccelerometerIsRunning(lua_State* state) {
        lua_pushboolean(state, self(state)->accelerometer_running);
        return 1;
    }

    static int cReadAccelerometer(lua_State* state) {
        Impl* runtime = self(state);
        const bool available = runtime->accelerometer_running &&
            runtime->accelerometer_valid;
        lua_pushnumber(state, available ? runtime->accelerometer_x : 0.0f);
        lua_pushnumber(state, available ? runtime->accelerometer_y : 0.0f);
        lua_pushnumber(state, available ? runtime->accelerometer_z : 1.0f);
        return 3;
    }

    static int cGetCurrentTime(lua_State* state) {
        lua_pushinteger(state, static_cast<lua_Integer>(self(state)->now_ms));
        return 1;
    }

    static int cGetElapsedTime(lua_State* state) {
        const Impl* runtime = self(state);
        const uint32_t elapsed = runtime->now_ms - runtime->elapsed_reset_ms;
        lua_pushnumber(state, static_cast<lua_Number>(elapsed) /
            static_cast<lua_Number>(1000));
        return 1;
    }

    static int cResetElapsedTime(lua_State* state) {
        Impl* runtime = self(state);
        const uint32_t elapsed = runtime->now_ms - runtime->elapsed_reset_ms;
        runtime->elapsed_reset_ms = runtime->now_ms;
        lua_pushnumber(state, static_cast<lua_Number>(elapsed) /
            static_cast<lua_Number>(1000));
        return 1;
    }

    static uint8_t checkButtonMask(lua_State* state, int index) {
        if (lua_type(state, index) == LUA_TNUMBER) {
            // Preserve the documented Playdate bit-mask API.  Combined masks
            // are valid, so numeric values must not be interpreted as PICO-8
            // button indices.
            return static_cast<uint8_t>(luaL_checkinteger(state, index));
        }

        if (lua_type(state, index) != LUA_TSTRING) {
            luaL_typeerror(state, index, "button mask or name");
            return 0;
        }

        const char* name = lua_tostring(state, index);
        char normalized[32]{};
        size_t length = 0;
        for (const char* cursor = name; *cursor && length + 1 < sizeof(normalized);
             ++cursor) {
            char value = *cursor;
            if (value >= 'A' && value <= 'Z') value = static_cast<char>(value + ('a' - 'A'));
            if ((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9')) {
                normalized[length++] = value;
            }
        }

        const char* token = normalized;
        if (!std::strncmp(token, "playdate", 8)) token += 8;
        if (!std::strncmp(token, "kbutton", 7)) token += 7;
        else if (!std::strncmp(token, "button", 6)) token += 6;
        if (!std::strncmp(token, "dpad", 4)) token += 4;

        if (!std::strcmp(token, "left")) return 0x04;
        if (!std::strcmp(token, "right")) return 0x08;
        if (!std::strcmp(token, "up") || !std::strcmp(token, "top")) return 0x01;
        if (!std::strcmp(token, "down")) return 0x02;
        if (!std::strcmp(token, "a")) return 0x20;
        if (!std::strcmp(token, "b")) return 0x10;

        luaL_argerror(state, index,
                      "unknown button name (expected left/right/up/down/a/b)");
        return 0;
    }

    static int cButtonJustPressed(lua_State* state) {
        Impl* runtime = self(state);
        const uint8_t button = checkButtonMask(state, 1);
        lua_pushboolean(state, (runtime->pressed_buttons & button) != 0);
        return 1;
    }

    static int cButtonIsPressed(lua_State* state) {
        Impl* runtime = self(state);
        const uint8_t button = checkButtonMask(state, 1);
        lua_pushboolean(state, (runtime->held_buttons & button) != 0);
        return 1;
    }

    static int cButtonJustReleased(lua_State* state) {
        Impl* runtime = self(state);
        const uint8_t button = checkButtonMask(state, 1);
        const uint8_t released = static_cast<uint8_t>(
            runtime->previous_held_buttons & ~runtime->held_buttons);
        lua_pushboolean(state, (released & button) != 0);
        return 1;
    }

    static int cGetButtonState(lua_State* state) {
        Impl* runtime = self(state);
        const uint8_t released = static_cast<uint8_t>(
            runtime->previous_held_buttons & ~runtime->held_buttons);
        lua_pushinteger(state, runtime->held_buttons);
        lua_pushinteger(state, runtime->pressed_buttons);
        lua_pushinteger(state, released);
        return 3;
    }

    static constexpr int64_t kPlaydateEpochUnixSeconds = 946684800LL;

    static void pushTimeTable(lua_State* state, const std::tm* value,
                              int millisecond) {
        lua_newtable(state);
        auto set = [&](const char* name, int number) {
            lua_pushinteger(state, number); lua_setfield(state, -2, name);
        };
        if (value) {
            set("year", value->tm_year + 1900); set("month", value->tm_mon + 1);
            set("day", value->tm_mday);
            // C uses Sunday=0 while Playdate exposes Monday=1...Sunday=7.
            set("weekday", ((value->tm_wday + 6) % 7) + 1);
            set("hour", value->tm_hour);
            set("minute", value->tm_min); set("second", value->tm_sec);
            set("millisecond", std::clamp(millisecond, 0, 999));
        }
    }

    static int readTimeField(lua_State* state, int table, const char* name,
                             int fallback) {
        table = lua_absindex(state, table);
        lua_getfield(state, table, name);
        const int result = lua_isnumber(state, -1)
            ? static_cast<int>(lua_tointeger(state, -1)) : fallback;
        lua_pop(state, 1);
        return result;
    }

    // Days relative to 1970-01-01 in the proleptic Gregorian calendar.  This
    // keeps epochFromGMTTime independent of the device's local TZ setting.
    static int64_t daysFromCivil(int year, unsigned month, unsigned day) {
        year -= month <= 2U;
        const int era = (year >= 0 ? year : year - 399) / 400;
        const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
        const unsigned adjusted_month = static_cast<unsigned>(
            static_cast<int>(month) + (month > 2U ? -3 : 9));
        const unsigned day_of_year =
            (153U * adjusted_month + 2U) / 5U + day - 1U;
        const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U -
            year_of_era / 100U + day_of_year;
        return static_cast<int64_t>(era) * 146097LL +
            static_cast<int64_t>(day_of_era) - 719468LL;
    }

    static void currentWallClock(timeval& now) {
        if (gettimeofday(&now, nullptr) != 0) {
            now.tv_sec = std::time(nullptr);
            now.tv_usec = 0;
        }
    }

    static int cGetSecondsSinceEpoch(lua_State* state) {
        timeval now{};
        currentWallClock(now);
        lua_pushinteger(state, static_cast<lua_Integer>(
            static_cast<int64_t>(now.tv_sec) - kPlaydateEpochUnixSeconds));
        lua_pushinteger(state, static_cast<lua_Integer>(now.tv_usec / 1000));
        return 2;
    }

    static int pushCurrentTime(lua_State* state, bool gmt) {
        timeval now{};
        currentWallClock(now);
        std::tm value{};
        const std::time_t seconds = now.tv_sec;
        const std::tm* converted = gmt
            ? gmtime_r(&seconds, &value) : localtime_r(&seconds, &value);
        pushTimeTable(state, converted, static_cast<int>(now.tv_usec / 1000));
        return 1;
    }

    static int cGetTime(lua_State* state) {
        return pushCurrentTime(state, false);
    }

    static int cGetGmtTime(lua_State* state) {
        return pushCurrentTime(state, true);
    }

    static int cEpochFromTime(lua_State* state) {
        luaL_checktype(state, 1, LUA_TTABLE);
        std::tm value{};
        value.tm_year = readTimeField(state, 1, "year", 2000) - 1900;
        value.tm_mon = readTimeField(state, 1, "month", 1) - 1;
        value.tm_mday = readTimeField(state, 1, "day", 1);
        value.tm_hour = readTimeField(state, 1, "hour", 0);
        value.tm_min = readTimeField(state, 1, "minute", 0);
        value.tm_sec = readTimeField(state, 1, "second", 0);
        value.tm_isdst = -1;
        const std::time_t seconds = std::mktime(&value);
        lua_pushinteger(state, static_cast<lua_Integer>(
            static_cast<int64_t>(seconds) - kPlaydateEpochUnixSeconds));
        lua_pushinteger(state, readTimeField(state, 1, "millisecond", 0));
        return 2;
    }

    static int cEpochFromGmtTime(lua_State* state) {
        luaL_checktype(state, 1, LUA_TTABLE);
        const int year = readTimeField(state, 1, "year", 2000);
        const unsigned month = static_cast<unsigned>(std::clamp(
            readTimeField(state, 1, "month", 1), 1, 12));
        const unsigned day = static_cast<unsigned>(std::clamp(
            readTimeField(state, 1, "day", 1), 1, 31));
        const int64_t unix_seconds = daysFromCivil(year, month, day) * 86400LL +
            static_cast<int64_t>(readTimeField(state, 1, "hour", 0)) * 3600LL +
            static_cast<int64_t>(readTimeField(state, 1, "minute", 0)) * 60LL +
            static_cast<int64_t>(readTimeField(state, 1, "second", 0));
        lua_pushinteger(state, static_cast<lua_Integer>(
            unix_seconds - kPlaydateEpochUnixSeconds));
        lua_pushinteger(state, readTimeField(state, 1, "millisecond", 0));
        return 2;
    }

    static int pushTimeFromEpoch(lua_State* state, bool gmt) {
        const int64_t playdate_seconds = luaL_checkinteger(state, 1);
        const int milliseconds = static_cast<int>(luaL_optinteger(state, 2, 0));
        const std::time_t seconds = static_cast<std::time_t>(
            playdate_seconds + kPlaydateEpochUnixSeconds);
        std::tm value{};
        const std::tm* converted = gmt
            ? gmtime_r(&seconds, &value) : localtime_r(&seconds, &value);
        pushTimeTable(state, converted, milliseconds);
        return 1;
    }

    static int cTimeFromEpoch(lua_State* state) {
        return pushTimeFromEpoch(state, false);
    }

    static int cGmtTimeFromEpoch(lua_State* state) {
        return pushTimeFromEpoch(state, true);
    }

    static int cGetReduceFlashing(lua_State* state) { lua_pushboolean(state, 1); return 1; }
    static int cIsCrankDocked(lua_State* state) {
        lua_pushboolean(state, self(state)->crank_docked);
        return 1;
    }
    static int cGetCrankPosition(lua_State* state) {
        lua_pushnumber(state, self(state)->crank_position);
        return 1;
    }
    static int cGetCrankChange(lua_State* state) {
        Impl* runtime = self(state);
        lua_pushnumber(state, runtime->crank_pending_change);
        lua_pushnumber(state, runtime->crank_pending_accelerated_change);
        runtime->crank_pending_change = 0.0f;
        runtime->crank_pending_accelerated_change = 0.0f;
        return 2;
    }
    static int cGetCrankTicks(lua_State* state) {
        Impl* runtime = self(state);
        const lua_Integer ticks_per_revolution = luaL_checkinteger(state, 1);
        if (ticks_per_revolution <= 0 || ticks_per_revolution > 100000) {
            return luaL_argerror(state, 1,
                "ticks per revolution must be between 1 and 100000");
        }
        const double scale = static_cast<double>(ticks_per_revolution) / 360.0;
        const double previous = runtime->crank_tick_previous_position * scale;
        const double current = runtime->crank_unwrapped_position * scale;
        const int64_t previous_tick = static_cast<int64_t>(std::floor(previous));
        const int64_t current_tick = static_cast<int64_t>(std::floor(current));
        const int64_t difference = current_tick - previous_tick;
        runtime->crank_tick_previous_position = runtime->crank_unwrapped_position;
        lua_pushinteger(state, static_cast<lua_Integer>(std::clamp<int64_t>(
            difference, std::numeric_limits<lua_Integer>::min(),
            std::numeric_limits<lua_Integer>::max())));
        return 1;
    }
    static int cSetCrankSoundsDisabled(lua_State* state) {
        Impl* runtime = self(state);
        const bool previous = runtime->crank_sounds_disabled;
        runtime->crank_sounds_disabled = lua_toboolean(state, 1) != 0;
        lua_pushboolean(state, previous);
        return 1;
    }
    static int cNoop(lua_State*) { return 0; }
    static int cDrawFps(lua_State*) { return 0; }
    static int cSetMenuImage(lua_State*) { return 0; }

    static bool pathHasSuffix(const char* path, const char* suffix) {
        if (!path || !suffix) return false;
        const size_t path_length = std::strlen(path);
        const size_t suffix_length = std::strlen(suffix);
        if (path_length < suffix_length) return false;
        const char* value = path + path_length - suffix_length;
        for (size_t index = 0; index < suffix_length; ++index) {
            char left = value[index];
            char right = suffix[index];
            if (left >= 'A' && left <= 'Z') {
                left = static_cast<char>(left + ('a' - 'A'));
            }
            if (right >= 'A' && right <= 'Z') {
                right = static_cast<char>(right + ('a' - 'A'));
            }
            if (left != right) return false;
        }
        return true;
    }

    bool pdaPath(char* output, size_t capacity, const char* requested) const {
        if (!package_mode || !requested || !requested[0] ||
            std::strstr(requested, "..") || requested[0] == '/') return false;
        char relative[176]{};
        std::snprintf(relative, sizeof(relative), "%s", requested);
        if (pathHasSuffix(relative, ".pda")) {
            return pdxJoinPath(output, capacity, package_info.path, relative);
        }
        const size_t length = std::strlen(relative);
        const char* extensions[] = {".wav", ".aiff", ".aif"};
        for (const char* extension : extensions) {
            const size_t suffix = std::strlen(extension);
            if (pathHasSuffix(relative, extension)) {
                relative[length - suffix] = '\0';
                break;
            }
        }
        return pdxJoinPath(output, capacity, package_info.path, relative, ".pda");
    }

    bool pdaDuration(const char* requested, float& seconds) const {
        seconds = 0.0f;
        char path[256]{};
        if (!pdaPath(path, sizeof(path), requested)) return false;
        FILE* file = std::fopen(path, "rb");
        if (!file) return false;
        std::fseek(file, 0, SEEK_END);
        const long length = std::ftell(file);
        std::rewind(file);
        uint8_t header[18]{};
        const bool header_ok = length > 16 &&
            std::fread(header, 1, sizeof(header), file) == sizeof(header) &&
            std::memcmp(header, "Playdate AUD", 12) == 0;
        std::fclose(file);
        if (!header_ok) return false;
        const uint32_t rate = readLe24(header + 12);
        const uint8_t format = header[15];
        const uint64_t payload = static_cast<uint64_t>(length - 16);
        uint64_t frames = 0;
        if (rate < 8000U || rate > 48000U || format > 5U) return false;
        if (format <= 3U) {
            const uint32_t bytes_per_sample = format >= 2U ? 2U : 1U;
            const uint32_t channels = (format & 1U) ? 2U : 1U;
            const uint32_t bytes_per_frame = bytes_per_sample * channels;
            if (!bytes_per_frame || payload % bytes_per_frame != 0U) return false;
            frames = payload / bytes_per_frame;
        } else {
            const uint16_t block_align = readLe16(header + 16);
            const uint16_t channel_headers = format == 5U ? 8U : 4U;
            if (payload < 2U || block_align <= channel_headers ||
                (payload - 2U) % block_align != 0U) return false;
            const uint64_t blocks = (payload - 2U) / block_align;
            const uint64_t per_block = format == 5U
                ? 1U + block_align - channel_headers
                : 1U + (block_align - channel_headers) * 2U;
            frames = blocks * per_block;
        }
        seconds = static_cast<float>(frames) / static_cast<float>(rate);
        return std::isfinite(seconds) && seconds >= 0.0f;
    }

    bool loadPda(const char* requested, int16_t*& samples, uint32_t& frames,
                 uint32_t& sample_rate) {
        samples = nullptr; frames = 0; sample_rate = 0;
        char path[256]{};
        if (!pdaPath(path, sizeof(path), requested)) return false;
        FILE* file = std::fopen(path, "rb");
        if (!file) return false;
        std::fseek(file, 0, SEEK_END);
        const long length = std::ftell(file);
        std::rewind(file);
        uint8_t header[16]{};
        if (length <= 16 || std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
            std::memcmp(header, "Playdate AUD", 12) != 0) {
            std::fclose(file);
            return false;
        }
        sample_rate = readLe24(header + 12);
        const uint8_t format = header[15];
        const size_t payload_size = static_cast<size_t>(length - 16);
        if (sample_rate < 8000U || sample_rate > 48000U ||
            payload_size > 16U * 1024U * 1024U || format > 5U) {
            std::fclose(file);
            return false;
        }

        // PDA PCM encodings 0..3 are unsigned 8-bit/signed 16-bit, mono/stereo.
        // Pogopo's audio path is mono, so stereo assets are downmixed while
        // streaming from SD instead of allocating a second full-size buffer.
        if (format <= 3U) {
            const bool sixteen_bit = format >= 2U;
            const bool stereo = (format & 1U) != 0U;
            const size_t bytes_per_sample = sixteen_bit ? 2U : 1U;
            const size_t bytes_per_frame = bytes_per_sample * (stereo ? 2U : 1U);
            if (payload_size == 0U || payload_size % bytes_per_frame != 0U) {
                std::fclose(file);
                return false;
            }
            const size_t decoded_frames = payload_size / bytes_per_frame;
            if (decoded_frames > 2U * 1024U * 1024U) {
                std::fclose(file);
                return false;
            }
            frames = static_cast<uint32_t>(decoded_frames);
            samples = static_cast<int16_t*>(heap_caps_malloc(
                static_cast<size_t>(frames) * sizeof(int16_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!samples) samples = static_cast<int16_t*>(heap_caps_malloc(
                static_cast<size_t>(frames) * sizeof(int16_t), MALLOC_CAP_8BIT));
            uint8_t* chunk = static_cast<uint8_t*>(heap_caps_malloc(
                4096U, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (!chunk) chunk = static_cast<uint8_t*>(heap_caps_malloc(4096U, MALLOC_CAP_8BIT));
            bool ok = samples && chunk;
            uint32_t output = 0U;
            while (ok && output < frames) {
                const size_t remaining = static_cast<size_t>(frames - output);
                const size_t chunk_frames = std::min<size_t>(remaining, 4096U / bytes_per_frame);
                const size_t chunk_bytes = chunk_frames * bytes_per_frame;
                if (std::fread(chunk, 1, chunk_bytes, file) != chunk_bytes) {
                    ok = false;
                    break;
                }
                for (size_t index = 0; index < chunk_frames; ++index) {
                    const uint8_t* source = chunk + index * bytes_per_frame;
                    int32_t left = sixteen_bit
                        ? static_cast<int16_t>(readLe16(source))
                        : (static_cast<int32_t>(source[0]) - 128) * 256;
                    int32_t mixed = left;
                    if (stereo) {
                        const uint8_t* right_source = source + bytes_per_sample;
                        const int32_t right = sixteen_bit
                            ? static_cast<int16_t>(readLe16(right_source))
                            : (static_cast<int32_t>(right_source[0]) - 128) * 256;
                        mixed = (left + right) / 2;
                    }
                    samples[output++] = static_cast<int16_t>(mixed);
                }
            }
            if (chunk) heap_caps_free(chunk);
            std::fclose(file);
            if (!ok) {
                if (samples) heap_caps_free(samples);
                samples = nullptr; frames = 0;
            }
            return ok;
        }

        // Formats 4 and 5 use IMA ADPCM blocks. Each channel starts with a
        // 4-byte predictor/index header. In Playdate stereo PDA data every
        // following byte contains the left sample in its high nibble and the
        // right sample in its low nibble.
        const bool stereo = format == 5U;
        const uint16_t channel_header_bytes = stereo ? 8U : 4U;
        uint8_t block_bytes[2]{};
        if (payload_size < 6U || std::fread(block_bytes, 1, 2, file) != 2) {
            std::fclose(file);
            return false;
        }
        const uint16_t block_align = readLe16(block_bytes);
        if (block_align <= channel_header_bytes || block_align > 4096U ||
            (payload_size - 2U) % block_align != 0U) {
            std::fclose(file);
            return false;
        }
        const uint32_t block_count = static_cast<uint32_t>((payload_size - 2U) / block_align);
        const uint32_t samples_per_block = stereo
            ? 1U + (block_align - channel_header_bytes)
            : 1U + (block_align - channel_header_bytes) * 2U;
        const uint64_t total_frames = static_cast<uint64_t>(block_count) * samples_per_block;
        if (block_count == 0 || total_frames > 4U * 1024U * 1024U) {
            std::fclose(file);
            return false;
        }
        frames = static_cast<uint32_t>(total_frames);
        samples = static_cast<int16_t*>(heap_caps_malloc(
            static_cast<size_t>(frames) * sizeof(int16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        uint8_t* block = static_cast<uint8_t*>(heap_caps_malloc(
            block_align, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!block) block = static_cast<uint8_t*>(heap_caps_malloc(block_align, MALLOC_CAP_8BIT));
        if (!samples || !block) {
            if (samples) heap_caps_free(samples);
            if (block) heap_caps_free(block);
            samples = nullptr; frames = 0;
            std::fclose(file);
            return false;
        }
        static constexpr int step_table[89] = {
            7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
            50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,
            230,253,279,307,337,371,408,449,494,544,598,658,724,796,
            876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,
            2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,
            7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,
            20350,22385,24623,27086,29794,32767};
        static constexpr int index_table[16] = {
            -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
        uint32_t output = 0;
        bool ok = true;
        auto decodeNibble = [&](int nibble, int& predictor, int& step_index) {
            const int step = step_table[step_index];
            const int delta = (((nibble & 7) * 2 + 1) * step) >> 3;
            predictor += (nibble & 8) ? -delta : delta;
            predictor = std::clamp(predictor, -32768, 32767);
            step_index = std::clamp(step_index + index_table[nibble], 0, 88);
            return static_cast<int16_t>(predictor);
        };
        for (uint32_t block_index = 0; block_index < block_count; ++block_index) {
            if (std::fread(block, 1, block_align, file) != block_align) {
                ok = false; break;
            }
            int predictor[2] = {
                static_cast<int16_t>(readLe16(block)),
                stereo ? static_cast<int16_t>(readLe16(block + 4U)) : 0,
            };
            int step_index[2] = {
                std::clamp<int>(block[2], 0, 88),
                stereo ? std::clamp<int>(block[6], 0, 88) : 0,
            };
            samples[output++] = static_cast<int16_t>(stereo
                ? (predictor[0] + predictor[1]) / 2 : predictor[0]);
            if (!stereo) {
                for (uint16_t byte_index = 4; byte_index < block_align; ++byte_index) {
                    for (int shift : {0, 4}) {
                        const int nibble = (block[byte_index] >> shift) & 0x0F;
                        samples[output++] = decodeNibble(
                            nibble, predictor[0], step_index[0]);
                    }
                }
                continue;
            }
            for (uint16_t byte_index = 8U; byte_index < block_align;
                 ++byte_index) {
                const uint8_t encoded = block[byte_index];
                const int16_t left = decodeNibble(
                    (encoded >> 4U) & 0x0FU, predictor[0], step_index[0]);
                const int16_t right = decodeNibble(
                    encoded & 0x0FU, predictor[1], step_index[1]);
                samples[output++] = static_cast<int16_t>(
                    (static_cast<int32_t>(left) +
                     static_cast<int32_t>(right)) / 2);
            }
        }
        heap_caps_free(block);
        std::fclose(file);
        if (!ok || output != frames) {
            heap_caps_free(samples);
            samples = nullptr; frames = 0;
            return false;
        }
        return true;
    }

    void clearSoundCache() {
        for (CachedSound& cached : sound_cache) {
            if (cached.samples) heap_caps_free(cached.samples);
            cached = {};
        }
        sound_cache_bytes = 0;
    }

    int cacheSound(const char* requested) {
        if (!requested || !requested[0]) return -1;
        char resolved_path[384]{};
        if (!pdaPath(resolved_path, sizeof(resolved_path), requested)) return -1;
        for (size_t index = 0; index < sound_cache.size(); ++index) {
            if (sound_cache[index].samples &&
                !std::strcmp(sound_cache[index].path, resolved_path)) {
                return static_cast<int>(index);
            }
        }
        struct stat source_info{};
        if (stat(resolved_path, &source_info) != 0 ||
            source_info.st_size <= 0 ||
            static_cast<uint64_t>(source_info.st_size) >
                kMaximumCachedSoundBytes) {
            return -1;
        }

        size_t free_index = sound_cache.size();
        for (size_t index = 0; index < sound_cache.size(); ++index) {
            if (!sound_cache[index].samples) {
                free_index = index;
                break;
            }
        }
        if (free_index == sound_cache.size()) return -1;

        int16_t* samples = nullptr;
        uint32_t frames = 0;
        uint32_t sample_rate = 0;
        if (!loadPda(requested, samples, frames, sample_rate)) return -1;
        const size_t bytes = static_cast<size_t>(frames) * sizeof(int16_t);
        // Large effects and music-like samples should be decoded only when
        // played.  Caching them and then cloning for the audio owner doubles
        // peak PSRAM use; that can starve image/JSON loading even when the PDX
        // itself is small.
        if (bytes > kMaximumCachedSoundBytes ||
            bytes > kMaximumSoundCacheBytes - sound_cache_bytes) {
            heap_caps_free(samples);
            return -1;
        }

        CachedSound& cached = sound_cache[free_index];
        cached.samples = samples;
        cached.frames = frames;
        cached.sample_rate = sample_rate;
        std::snprintf(cached.path, sizeof(cached.path), "%s", resolved_path);
        sound_cache_bytes += bytes;
        return static_cast<int>(free_index);
    }

    static bool musicPath(const char* path) {
        if (!path) return false;
        char window[6]{};
        size_t length = 0;
        for (const char* cursor = path; *cursor; ++cursor) {
            char value = *cursor;
            if (value >= 'A' && value <= 'Z') {
                value = static_cast<char>(value + ('a' - 'A'));
            }
            if (length < 5U) {
                window[length++] = value;
            } else {
                std::memmove(window, window + 1, 4U);
                window[4] = value;
            }
            if (length == 5U && !std::memcmp(window, "music", 5U)) return true;
        }
        return false;
    }

    void preloadShortSounds(const char* relative = "", int depth = 0) {
        // Keep recursive directory state comfortably below the 8 KiB UI-task stack.
        if (!package_mode || depth >= 4 ||
            sound_cache_bytes >= kMaximumSoundCacheBytes) return;
        char directory_path[384]{};
        if (!pdxJoinPath(directory_path, sizeof(directory_path),
                         package_info.path, relative)) return;
        DIR* directory = opendir(directory_path);
        if (!directory) return;
        while (dirent* entry = readdir(directory)) {
            if (!std::strcmp(entry->d_name, ".") ||
                !std::strcmp(entry->d_name, "..")) continue;
            char child_relative[256]{};
            const int relative_length = relative && relative[0]
                ? std::snprintf(child_relative, sizeof(child_relative), "%s/%s",
                                relative, entry->d_name)
                : std::snprintf(child_relative, sizeof(child_relative), "%s",
                                entry->d_name);
            if (relative_length <= 0 ||
                static_cast<size_t>(relative_length) >= sizeof(child_relative)) {
                continue;
            }
            char child_path[384]{};
            if (!pdxJoinPath(child_path, sizeof(child_path), package_info.path,
                             child_relative)) continue;
            struct stat value{};
            if (stat(child_path, &value) != 0) continue;
            if (S_ISDIR(value.st_mode)) {
                if (!musicPath(child_relative)) {
                    preloadShortSounds(child_relative, depth + 1);
                }
                continue;
            }
            if (!S_ISREG(value.st_mode) || musicPath(child_relative)) continue;
            if (!pathHasSuffix(child_relative, ".pda")) continue;
            (void)cacheSound(child_relative);
        }
        closedir(directory);
    }

    bool cloneCachedSound(int index, int16_t*& samples, uint32_t& frames,
                          uint32_t& sample_rate) const {
        samples = nullptr;
        frames = 0;
        sample_rate = 0;
        if (index < 0 || static_cast<size_t>(index) >= sound_cache.size()) {
            return false;
        }
        const CachedSound& cached = sound_cache[static_cast<size_t>(index)];
        if (!cached.samples || cached.frames == 0) return false;
        const size_t bytes = static_cast<size_t>(cached.frames) * sizeof(int16_t);
        samples = static_cast<int16_t*>(heap_caps_malloc(
            bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!samples) {
            samples = static_cast<int16_t*>(heap_caps_malloc(
                bytes, MALLOC_CAP_8BIT));
        }
        if (!samples) return false;
        std::memcpy(samples, cached.samples, bytes);
        frames = cached.frames;
        sample_rate = cached.sample_rate;
        return true;
    }

    static int cSoundNew(lua_State* state) {
        Impl* runtime = self(state);
        const char* path = lua_type(state, 1) == LUA_TSTRING ? lua_tostring(state, 1) : "";
        auto* original = static_cast<Sound*>(luaL_testudata(state, 1, kSoundMetatable));
        auto* sound = static_cast<Sound*>(lua_newuserdatauv(state, sizeof(Sound), 0));
        new (sound) Sound{};
        if (original) *sound = *original;
        else {
            sound->effect = effectForPath(path);
            sound->music = musicPath(path);
            std::snprintf(sound->path, sizeof(sound->path), "%s", path ? path : "");
            if (!sound->music) sound->cache_index = runtime->cacheSound(path);
        }
        luaL_getmetatable(state, kSoundMetatable);
        lua_setmetatable(state, -2);
        return 1;
    }

    static int cFilePlayerNew(lua_State* state) {
        const char* path = lua_type(state, 1) == LUA_TSTRING ? lua_tostring(state, 1) : "";
        auto* original = static_cast<Sound*>(luaL_testudata(state, 1, kSoundMetatable));
        auto* sound = static_cast<Sound*>(lua_newuserdatauv(state, sizeof(Sound), 0));
        new (sound) Sound{};
        if (original) *sound = *original;
        else {
            sound->effect = effectForPath(path);
            std::snprintf(sound->path, sizeof(sound->path), "%s", path ? path : "");
        }
        // Long music tracks must not briefly enter the 2 MiB SFX cache.
        sound->music = true;
        sound->cache_index = -1;
        luaL_getmetatable(state, kSoundMetatable);
        lua_setmetatable(state, -2);
        return 1;
    }

    static int cSoundPlay(lua_State* state) {
        Impl* runtime = self(state);
        auto* sound = static_cast<Sound*>(luaL_checkudata(state, 1, kSoundMetatable));
        if (sound) {
            sound->playing = true;
            int16_t* samples = nullptr;
            uint32_t frames = 0, sample_rate = 0;
            const bool loaded = runtime->audio && sound->path[0] &&
                (runtime->cloneCachedSound(sound->cache_index, samples, frames,
                                           sample_rate) ||
                 runtime->loadPda(sound->path, samples, frames, sample_rate));
            if (loaded) {
                const float requested_rate = !lua_isnoneornil(state, 3)
                    ? static_cast<float>(luaL_checknumber(state, 3))
                    : sound->rate;
                sample_rate = static_cast<uint32_t>(std::clamp<float>(
                    sample_rate * std::max(0.01f, requested_rate),
                    4000.0f, 96000.0f));
                const uint8_t volume = static_cast<uint8_t>(std::clamp<int>(
                    static_cast<int>(std::lround(sound->volume * 100.0f)), 0, 100));
                if (!lua_isnoneornil(state, 2)) {
                    sound->loop = luaL_checkinteger(state, 2) == 0;
                }
                if (sound->music) {
                    runtime->audio->playMusicPcmOwned(samples, frames, sample_rate,
                                                      volume, sound->loop);
                } else {
                    runtime->audio->playPcmOwned(samples, frames, sample_rate, volume);
                }
            } else if (!sound->music && runtime->audio) {
                runtime->audio->play(sound->effect);
            }
        }
        lua_pushboolean(state, sound != nullptr);
        return 1;
    }

    static int cSoundStop(lua_State* state) {
        Impl* runtime = self(state);
        auto* sound = static_cast<Sound*>(luaL_checkudata(state, 1, kSoundMetatable));
        if (sound) {
            sound->playing = false;
            if (sound->music && runtime->audio) runtime->audio->stopMusicPcm();
        }
        return 0;
    }

    static int cSoundPause(lua_State* state) { return cSoundStop(state); }
    static int cSoundIsPlaying(lua_State* state) {
        auto* sound = static_cast<Sound*>(luaL_checkudata(state, 1, kSoundMetatable));
        lua_pushboolean(state, sound && sound->playing);
        return 1;
    }

    static int cSoundSetVolume(lua_State* state) {
        auto* sound = static_cast<Sound*>(luaL_checkudata(state, 1, kSoundMetatable));
        if (sound) {
            sound->volume = static_cast<float>(std::clamp<lua_Number>(
                luaL_checknumber(state, 2), static_cast<lua_Number>(0),
                static_cast<lua_Number>(1)));
        }
        return 0;
    }

    static int cSoundSetRate(lua_State* state) {
        auto* sound = static_cast<Sound*>(
            luaL_checkudata(state, 1, kSoundMetatable));
        if (sound) {
            sound->rate = static_cast<float>(std::clamp<lua_Number>(
                luaL_checknumber(state, 2), static_cast<lua_Number>(0.01),
                static_cast<lua_Number>(8.0)));
        }
        return 0;
    }

    static int cSoundGetLength(lua_State* state) {
        Impl* runtime = self(state);
        auto* sound = static_cast<Sound*>(
            luaL_checkudata(state, 1, kSoundMetatable));
        float seconds = 0.0f;
        if (!sound || !runtime->pdaDuration(sound->path, seconds)) {
            lua_pushnumber(state, 0);
            return 1;
        }
        lua_pushnumber(state, seconds / std::max(0.01f, sound->rate));
        return 1;
    }

    static int cSoundSetSample(lua_State* state) {
        auto* sound = static_cast<Sound*>(luaL_checkudata(state, 1, kSoundMetatable));
        auto* sample = static_cast<Sound*>(luaL_checkudata(state, 2, kSoundMetatable));
        if (sound && sample) *sound = *sample;
        return 0;
    }

    static int cSoundLoad(lua_State* state) {
        Impl* runtime = self(state);
        auto* sound = static_cast<Sound*>(luaL_checkudata(state, 1, kSoundMetatable));
        const char* path = luaL_checkstring(state, 2);
        if (sound) {
            sound->effect = effectForPath(path);
            sound->music = musicPath(path);
            std::snprintf(sound->path, sizeof(sound->path), "%s", path ? path : "");
            sound->cache_index = sound->music ? -1 : runtime->cacheSound(path);
        }
        lua_pushboolean(state, sound != nullptr);
        return 1;
    }

    static int noteNameToMidi(const char* name, float& midi) {
        if (!name || !name[0]) return 0;
        int semitone = 0;
        switch (static_cast<char>(std::toupper(
                    static_cast<unsigned char>(name[0])))) {
            case 'C': semitone = 0; break;
            case 'D': semitone = 2; break;
            case 'E': semitone = 4; break;
            case 'F': semitone = 5; break;
            case 'G': semitone = 7; break;
            case 'A': semitone = 9; break;
            case 'B': semitone = 11; break;
            default: return 0;
        }
        const char* cursor = name + 1;
        if (*cursor == '#') { ++semitone; ++cursor; }
        else if (*cursor == 'b' || *cursor == 'B') { --semitone; ++cursor; }
        char* end = nullptr;
        const long octave = std::strtol(cursor, &end, 10);
        if (end == cursor || *end != '\0' || octave < -1 || octave > 9) return 0;
        midi = static_cast<float>((octave + 1) * 12 + semitone);
        return 1;
    }

    static float midiToFrequency(float midi) {
        return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
    }

    static audio::Waveform synthWaveform(int waveform) {
        switch (waveform) {
            case 0: return audio::Waveform::Square;
            case 1: return audio::Waveform::Triangle;
            case 2: return audio::Waveform::Sine;
            case 3: return audio::Waveform::Noise;
            case 4: return audio::Waveform::Sawtooth;
            // The three Playdate Organ-style generators do not have a direct
            // equivalent in Pogopo's small mixer. Keep them audible with the
            // closest inexpensive oscillator instead of silently dropping notes.
            case 5: return audio::Waveform::Sawtooth;
            case 6: return audio::Waveform::Square;
            case 7: return audio::Waveform::Triangle;
            default: return audio::Waveform::Sine;
        }
    }

    static int cSynthNew(lua_State* state) {
        const int argument = lua_istable(state, 1) ? 2 : 1;
        auto* original = static_cast<Synth*>(
            luaL_testudata(state, argument, kSynthMetatable));
        auto* synth = static_cast<Synth*>(lua_newuserdatauv(state, sizeof(Synth), 0));
        new (synth) Synth{};
        if (original) {
            *synth = *original;
            synth->voice_token = 0;
            synth->ends_at_ms = 0;
            synth->playing = false;
            synth->indefinite = false;
        } else if (lua_isnumber(state, argument)) {
            synth->waveform = static_cast<int>(std::clamp<lua_Integer>(
                lua_tointeger(state, argument), 0, 7));
        }
        luaL_getmetatable(state, kSynthMetatable);
        lua_setmetatable(state, -2);
        return 1;
    }

    static int cSynthCopy(lua_State* state) {
        luaL_checkudata(state, 1, kSynthMetatable);
        lua_pushvalue(state, 1);
        return cSynthNew(state);
    }

    static int cSynthPlay(lua_State* state, bool midi_pitch) {
        Impl* runtime = self(state);
        auto* synth = static_cast<Synth*>(
            luaL_checkudata(state, 1, kSynthMetatable));
        float frequency = 0.0f;
        if (lua_type(state, 2) == LUA_TSTRING) {
            float midi = 0.0f;
            if (!noteNameToMidi(lua_tostring(state, 2), midi)) {
                return luaL_argerror(state, 2, "invalid note name");
            }
            frequency = midiToFrequency(midi + synth->transpose);
        } else {
            const float pitch = static_cast<float>(luaL_checknumber(state, 2));
            frequency = midi_pitch
                ? midiToFrequency(pitch + synth->transpose)
                : pitch * std::pow(2.0f, synth->transpose / 12.0f);
        }
        if (frequency <= 0.0f) {
            if (synth->voice_token && runtime->audio) {
                runtime->audio->stopSynthTone(synth->voice_token, 0);
            }
            synth->voice_token = 0;
            synth->playing = false;
            synth->indefinite = false;
            lua_pushboolean(state, 1);
            return 1;
        }

        const float velocity = static_cast<float>(std::clamp<lua_Number>(
            luaL_optnumber(state, 3, 1.0), static_cast<lua_Number>(0),
            static_cast<lua_Number>(1)));
        const bool indefinite = lua_isnoneornil(state, 4);
        const float requested_seconds = indefinite
            ? 60.0f
            : static_cast<float>(std::max<lua_Number>(
                  luaL_checknumber(state, 4), static_cast<lua_Number>(0.001)));
        const uint16_t duration_ms = static_cast<uint16_t>(std::clamp<int>(
            static_cast<int>(std::lround(requested_seconds * 1000.0f)), 1, 60000));
        const uint16_t attack_ms = static_cast<uint16_t>(std::clamp<int>(
            static_cast<int>(std::lround(synth->attack * 1000.0f)), 0, 60000));
        const uint16_t decay_ms = static_cast<uint16_t>(std::clamp<int>(
            static_cast<int>(std::lround(synth->decay * 1000.0f)), 0, 60000));
        const uint16_t sustain_q15 = static_cast<uint16_t>(std::clamp<int>(
            static_cast<int>(std::lround(synth->sustain * 32767.0f)), 0, 32767));
        const uint16_t release_ms = static_cast<uint16_t>(std::clamp<int>(
            static_cast<int>(std::lround(synth->release * 1000.0f)), 0, 60000));
        const float source_volume = (synth->left_volume + synth->right_volume) * 0.5f;
        const uint8_t volume = static_cast<uint8_t>(std::clamp<int>(
            static_cast<int>(std::lround(source_volume * velocity * 100.0f)),
            0, 100));

        if (synth->voice_token && runtime->audio) {
            runtime->audio->stopSynthTone(synth->voice_token, 0);
        }
        synth->voice_token = runtime->audio
            ? runtime->audio->playSynthTone(
                  static_cast<uint16_t>(std::clamp<int>(
                      static_cast<int>(std::lround(frequency)), 20, 16000)),
                  duration_ms, volume, synthWaveform(synth->waveform),
                  attack_ms, decay_ms, sustain_q15, release_ms)
            : 1;
        synth->playing = synth->voice_token != 0;
        synth->indefinite = synth->playing && indefinite;
        synth->ends_at_ms = synth->indefinite
            ? 0
            : runtime->now_ms + duration_ms;
        lua_pushboolean(state, synth->playing);
        return 1;
    }

    static int cSynthPlayNote(lua_State* state) { return cSynthPlay(state, false); }
    static int cSynthPlayMidiNote(lua_State* state) { return cSynthPlay(state, true); }

    static int cSynthNoteOff(lua_State* state) {
        Impl* runtime = self(state);
        auto* synth = static_cast<Synth*>(
            luaL_checkudata(state, 1, kSynthMetatable));
        const uint16_t release_ms = static_cast<uint16_t>(std::clamp<int>(
            static_cast<int>(std::lround(synth->release * 1000.0f)), 0, 60000));
        if (synth->voice_token && runtime->audio) {
            runtime->audio->stopSynthTone(synth->voice_token, release_ms);
        }
        synth->indefinite = false;
        synth->ends_at_ms = runtime->now_ms + release_ms;
        if (release_ms == 0) {
            synth->voice_token = 0;
            synth->playing = false;
        }
        return 0;
    }

    static int cSynthStop(lua_State* state) {
        Impl* runtime = self(state);
        auto* synth = static_cast<Synth*>(
            luaL_checkudata(state, 1, kSynthMetatable));
        if (synth->voice_token && runtime->audio) {
            runtime->audio->stopSynthTone(synth->voice_token, 0);
        }
        synth->voice_token = 0;
        synth->ends_at_ms = 0;
        synth->playing = false;
        synth->indefinite = false;
        return 0;
    }

    static int cSynthGc(lua_State* state) { return cSynthStop(state); }

    static int cSynthIsPlaying(lua_State* state) {
        Impl* runtime = self(state);
        auto* synth = static_cast<Synth*>(
            luaL_checkudata(state, 1, kSynthMetatable));
        if (synth->playing && !synth->indefinite &&
            static_cast<int32_t>(runtime->now_ms - synth->ends_at_ms) >= 0) {
            synth->voice_token = 0;
            synth->playing = false;
        }
        lua_pushboolean(state, synth->playing);
        return 1;
    }

    static int cSynthSetWaveform(lua_State* state) {
        auto* synth = static_cast<Synth*>(
            luaL_checkudata(state, 1, kSynthMetatable));
        if (lua_isnumber(state, 2)) {
            synth->waveform = static_cast<int>(std::clamp<lua_Integer>(
                lua_tointeger(state, 2), 0, 7));
        }
        return 0;
    }

    static int cSynthSetVolume(lua_State* state) {
        auto* synth = static_cast<Synth*>(
            luaL_checkudata(state, 1, kSynthMetatable));
        synth->left_volume = static_cast<float>(std::clamp<lua_Number>(
            luaL_checknumber(state, 2), static_cast<lua_Number>(0),
            static_cast<lua_Number>(1)));
        synth->right_volume = static_cast<float>(std::clamp<lua_Number>(
            luaL_optnumber(state, 3, synth->left_volume),
            static_cast<lua_Number>(0), static_cast<lua_Number>(1)));
        return 0;
    }

    static int cSynthGetVolume(lua_State* state) {
        auto* synth = static_cast<Synth*>(
            luaL_checkudata(state, 1, kSynthMetatable));
        lua_pushnumber(state, (synth->left_volume + synth->right_volume) * 0.5f);
        return 1;
    }

    static int cSynthSetAdsr(lua_State* state) {
        auto* synth = static_cast<Synth*>(
            luaL_checkudata(state, 1, kSynthMetatable));
        synth->attack = static_cast<float>(std::max<lua_Number>(0,
            luaL_checknumber(state, 2)));
        synth->decay = static_cast<float>(std::max<lua_Number>(0,
            luaL_checknumber(state, 3)));
        synth->sustain = static_cast<float>(std::clamp<lua_Number>(
            luaL_checknumber(state, 4), static_cast<lua_Number>(0),
            static_cast<lua_Number>(1)));
        synth->release = static_cast<float>(std::max<lua_Number>(0,
            luaL_checknumber(state, 5)));
        return 0;
    }

    static int cSynthSetAttack(lua_State* state) {
        auto* synth = static_cast<Synth*>(luaL_checkudata(state, 1, kSynthMetatable));
        synth->attack = static_cast<float>(std::max<lua_Number>(0, luaL_checknumber(state, 2)));
        return 0;
    }
    static int cSynthSetDecay(lua_State* state) {
        auto* synth = static_cast<Synth*>(luaL_checkudata(state, 1, kSynthMetatable));
        synth->decay = static_cast<float>(std::max<lua_Number>(0, luaL_checknumber(state, 2)));
        return 0;
    }
    static int cSynthSetSustain(lua_State* state) {
        auto* synth = static_cast<Synth*>(luaL_checkudata(state, 1, kSynthMetatable));
        synth->sustain = static_cast<float>(std::clamp<lua_Number>(
            luaL_checknumber(state, 2), static_cast<lua_Number>(0),
            static_cast<lua_Number>(1)));
        return 0;
    }
    static int cSynthSetRelease(lua_State* state) {
        auto* synth = static_cast<Synth*>(luaL_checkudata(state, 1, kSynthMetatable));
        synth->release = static_cast<float>(std::max<lua_Number>(0, luaL_checknumber(state, 2)));
        return 0;
    }
    static int cSynthClearEnvelope(lua_State* state) {
        auto* synth = static_cast<Synth*>(luaL_checkudata(state, 1, kSynthMetatable));
        synth->attack = 0.0f;
        synth->decay = 0.0f;
        synth->sustain = 1.0f;
        synth->release = 0.0f;
        return 0;
    }
    static int cSynthGetEnvelope(lua_State* state) {
        luaL_checkudata(state, 1, kSynthMetatable);
        lua_pushvalue(state, 1);
        return 1;
    }
    static int cSynthSetTranspose(lua_State* state) {
        auto* synth = static_cast<Synth*>(luaL_checkudata(state, 1, kSynthMetatable));
        synth->transpose = static_cast<float>(std::clamp<lua_Number>(
            luaL_checknumber(state, 2), static_cast<lua_Number>(-96),
            static_cast<lua_Number>(96)));
        return 0;
    }
    static int cSynthSetParameter(lua_State* state) {
        luaL_checkudata(state, 1, kSynthMetatable);
        (void)luaL_checkinteger(state, 2);
        (void)luaL_checknumber(state, 3);
        lua_pushboolean(state, 1);
        return 1;
    }
    static int cSynthGetParameterCount(lua_State* state) {
        auto* synth = static_cast<Synth*>(luaL_checkudata(state, 1, kSynthMetatable));
        const int count = synth->waveform == 0 ? 1 :
            (synth->waveform >= 5 ? 2 : 0);
        lua_pushinteger(state, count);
        return 1;
    }
    static int cSynthSetWavetable(lua_State* state) {
        luaL_checkudata(state, 1, kSynthMetatable);
        lua_pushboolean(state, 1);
        return 1;
    }

    static int cSoundGetCurrentTime(lua_State* state) {
        lua_pushnumber(state, static_cast<lua_Number>(self(state)->now_ms) / 1000.0);
        return 1;
    }

    void pushTimerRegistry() { lua_rawgetp(lua, LUA_REGISTRYINDEX, &kTimerRegistryKey); }

    static int cTimerPause(lua_State* state) {
        luaL_checktype(state, 1, LUA_TTABLE);
        lua_pushboolean(state, 1); lua_setfield(state, 1, "paused");
        return 0;
    }
    static int cTimerStart(lua_State* state) {
        luaL_checktype(state, 1, LUA_TTABLE);
        lua_pushboolean(state, 0); lua_setfield(state, 1, "paused");
        return 0;
    }
    static int cTimerReset(lua_State* state) {
        luaL_checktype(state, 1, LUA_TTABLE);
        lua_pushnumber(state, 0); lua_setfield(state, 1, "currentTime");
        lua_pushnumber(state, 0); lua_setfield(state, 1, "elapsed");
        lua_pushboolean(state, 0); lua_setfield(state, 1, "removed");
        return 0;
    }
    static int cTimerRemove(lua_State* state) {
        luaL_checktype(state, 1, LUA_TTABLE);
        lua_pushboolean(state, 1); lua_setfield(state, 1, "removed");
        return 0;
    }

    int createTimer(lua_State* state, int duration_index, int callback_index,
                    bool callback_required = false) {
        const lua_Number duration = std::max<lua_Number>(1, luaL_checknumber(state, duration_index));
        const bool has_callback = lua_isfunction(state, callback_index);
        if (callback_required && !has_callback) {
            return luaL_argerror(state, callback_index, "function expected");
        }
        const lua_Number start_value = has_callback ? 0 :
            luaL_optnumber(state, callback_index, 0);
        const lua_Number end_value = has_callback ? 0 :
            luaL_optnumber(state, callback_index + 1, 0);
        lua_newtable(state);
        const int timer = lua_gettop(state);
        lua_pushnumber(state, duration); lua_setfield(state, timer, "duration");
        lua_pushnumber(state, 0); lua_setfield(state, timer, "elapsed");
        lua_pushnumber(state, 0); lua_setfield(state, timer, "currentTime");
        lua_pushnumber(state, duration); lua_setfield(state, timer, "timeLeft");
        lua_pushnumber(state, start_value); lua_setfield(state, timer, "startValue");
        lua_pushnumber(state, end_value); lua_setfield(state, timer, "endValue");
        lua_pushnumber(state, start_value); lua_setfield(state, timer, "value");
        lua_pushboolean(state, 0); lua_setfield(state, timer, "paused");
        lua_pushboolean(state, 0); lua_setfield(state, timer, "removed");
        lua_pushboolean(state, 0); lua_setfield(state, timer, "repeats");
        lua_pushboolean(state, 0); lua_setfield(state, timer, "reverses");
        lua_pushboolean(state, 1); lua_setfield(state, timer, "discardOnCompletion");
        if (has_callback) {
            lua_pushvalue(state, callback_index);
            lua_setfield(state, timer, "callback");
        } else if (lua_isfunction(state, callback_index + 2)) {
            lua_pushvalue(state, callback_index + 2);
            lua_setfield(state, timer, "easingFunction");
        }
        setFunction(timer, "pause", cTimerPause); setFunction(timer, "start", cTimerStart);
        setFunction(timer, "reset", cTimerReset); setFunction(timer, "remove", cTimerRemove);
        pushTimerRegistry();
        lua_pushinteger(state, next_timer_id++); lua_pushvalue(state, timer); lua_settable(state, -3);
        lua_pop(state, 1);
        return 1;
    }

    static int cTimerNew(lua_State* state) {
        return self(state)->createTimer(state, 1, 2, false);
    }
    static int cTimerAfter(lua_State* state) {
        return self(state)->createTimer(state, 1, 2, true);
    }

    static int cTimerAll(lua_State* state) {
        Impl* runtime = self(state);
        lua_newtable(state);
        const int result = lua_absindex(state, -1);
        runtime->pushTimerRegistry();
        const int registry = lua_absindex(state, -1);
        lua_Integer output = 1;
        lua_pushnil(state);
        while (lua_next(state, registry) != 0) {
            if (lua_istable(state, -1)) {
                lua_getfield(state, -1, "removed");
                const bool removed = lua_toboolean(state, -1) != 0;
                lua_pop(state, 1);
                if (!removed) {
                    lua_pushvalue(state, -1);
                    lua_rawseti(state, result, output++);
                }
            }
            lua_pop(state, 1);
        }
        lua_remove(state, registry);
        return 1;
    }

    bool updateTimers() {
        const int top = lua_gettop(lua);
        pushTimerRegistry();
        lua_pushnil(lua);
        while (lua_next(lua, -2) != 0) {
            if (!lua_istable(lua, -1)) { lua_pop(lua, 1); continue; }
            const int timer = lua_gettop(lua);
            lua_getfield(lua, timer, "removed"); const bool removed = lua_toboolean(lua, -1); lua_pop(lua,1);
            lua_getfield(lua, timer, "paused"); const bool paused = lua_toboolean(lua, -1); lua_pop(lua,1);
            if (removed || paused) { lua_pop(lua, 1); continue; }
            lua_getfield(lua, timer, "duration"); const lua_Number duration = std::max<lua_Number>(1,lua_tonumber(lua,-1)); lua_pop(lua,1);
            lua_getfield(lua, timer, "currentTime"); lua_Number elapsed = lua_tonumber(lua,-1) + frame_dt_ms; lua_pop(lua,1);
            lua_pushnumber(lua, elapsed); lua_setfield(lua, timer, "currentTime");
            lua_pushnumber(lua, elapsed); lua_setfield(lua, timer, "elapsed");
            lua_pushnumber(lua, std::max<lua_Number>(0, duration - elapsed));
            lua_setfield(lua, timer, "timeLeft");
            lua_getfield(lua, timer, "startValue");
            const lua_Number start_value = lua_tonumber(lua, -1); lua_pop(lua, 1);
            lua_getfield(lua, timer, "endValue");
            const lua_Number end_value = lua_tonumber(lua, -1); lua_pop(lua, 1);
            const lua_Number clamped_time = std::min(elapsed, duration);
            lua_Number timer_value = start_value +
                (end_value - start_value) * (clamped_time / duration);
            lua_getfield(lua, timer, "easingFunction");
            if (lua_isfunction(lua, -1)) {
                lua_pushnumber(lua, clamped_time);
                lua_pushnumber(lua, start_value);
                lua_pushnumber(lua, end_value - start_value);
                lua_pushnumber(lua, duration);
                if (lua_pcall(lua, 4, 1, 0) != LUA_OK) {
                    takeLuaError("timer easing"); lua_settop(lua, top); return false;
                }
                if (lua_isnumber(lua, -1)) timer_value = lua_tonumber(lua, -1);
                lua_pop(lua, 1);
            } else lua_pop(lua, 1);
            lua_pushnumber(lua, timer_value); lua_setfield(lua, timer, "value");
            lua_getfield(lua, timer, "updateCallback");
            if (lua_isfunction(lua, -1)) {
                lua_pushvalue(lua, timer);
                if (lua_pcall(lua, 1, 0, 0) != LUA_OK) { takeLuaError("timer update"); lua_settop(lua,top); return false; }
            } else lua_pop(lua,1);
            if (elapsed >= duration) {
                lua_getfield(lua, timer, "repeats"); const bool repeats = lua_toboolean(lua,-1); lua_pop(lua,1);
                if (repeats) {
                    elapsed = std::fmod(elapsed, duration);
                    lua_pushnumber(lua, elapsed); lua_setfield(lua, timer, "currentTime");
                    lua_pushnumber(lua, elapsed); lua_setfield(lua, timer, "elapsed");
                } else {
                    lua_pushboolean(lua, 1); lua_setfield(lua, timer, "removed");
                }
                lua_getfield(lua, timer, "callback");
                if (lua_isfunction(lua, -1)) {
                    lua_pushvalue(lua, timer);
                    if (lua_pcall(lua, 1, 0, 0) != LUA_OK) { takeLuaError("timer callback"); lua_settop(lua,top); return false; }
                } else lua_pop(lua,1);
                lua_getfield(lua, timer, "timerEndedCallback");
                if (lua_isfunction(lua, -1)) {
                    lua_pushvalue(lua, timer);
                    if (lua_pcall(lua, 1, 0, 0) != LUA_OK) { takeLuaError("timer ended"); lua_settop(lua,top); return false; }
                } else lua_pop(lua,1);
            }
            lua_pop(lua, 1);
        }
        lua_settop(lua, top);
        return true;
    }

    static int cUpdateTimers(lua_State* state) {
        Impl* runtime = self(state);
        if (!runtime->updateTimers()) return luaL_error(state, "%s", runtime->last_error);
        return 0;
    }

    static int cDisplaySize(lua_State* state) {
        const Impl* runtime = self(state);
        lua_pushinteger(state, 400 / runtime->display_scale);
        lua_pushinteger(state, 240 / runtime->display_scale);
        return 2;
    }

    static void sanitizeIdentifier(const char* value, char* output,
                                   size_t capacity) {
        if (!output || capacity == 0) return;
        size_t written = 0;
        for (const char* cursor = value && value[0] ? value : "external";
             *cursor && written + 1U < capacity; ++cursor) {
            const char c = *cursor;
            const bool safe = (c >= 'a' && c <= 'z') ||
                              (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '_' || c == '-';
            output[written++] = safe ? c : '_';
        }
        if (written == 0) {
            std::snprintf(output, capacity, "%s", "game");
            return;
        }
        output[written] = '\0';
    }

    static bool normalizeGamePath(const char* input, char* output,
                                  size_t capacity) {
        if (!output || capacity == 0) return false;
        output[0] = '\0';
        const char* cursor = input ? input : "";
        while (*cursor == '/') ++cursor;
        size_t written = 0;
        while (*cursor) {
            while (*cursor == '/') ++cursor;
            if (!*cursor) break;
            const char* segment = cursor;
            while (*cursor && *cursor != '/') ++cursor;
            const size_t length = static_cast<size_t>(cursor - segment);
            if (length == 1U && segment[0] == '.') continue;
            if (length == 2U && segment[0] == '.' && segment[1] == '.') {
                return false;
            }
            for (size_t i = 0; i < length; ++i) {
                const unsigned char c = static_cast<unsigned char>(segment[i]);
                if (c < 32U || c == '\\' || c == ':') return false;
            }
            const size_t needed = written + (written ? 1U : 0U) + length + 1U;
            if (needed > capacity) return false;
            if (written) output[written++] = '/';
            std::memcpy(output + written, segment, length);
            written += length;
            output[written] = '\0';
        }
        return true;
    }

    bool dataRoot(char* output, size_t capacity) const {
        if (!output || capacity == 0 || !storage || !storage->mounted()) {
            return false;
        }
        char identifier[96]{};
        const char* source = package_mode && package_info.bundle_id[0]
            ? package_info.bundle_id
            : (package_mode && package_info.name[0] ? package_info.name
               : (game == Game::Celeste ? "celeste" :
                  (game == Game::PDSnake ? "pdsnake" : "external")));
        sanitizeIdentifier(source, identifier, sizeof(identifier));
        const int length = std::snprintf(output, capacity, "%s/pogodate/data/%s",
                                         storage->mountPoint(), identifier);
        return length > 0 && static_cast<size_t>(length) < capacity;
    }

    static bool ensureDirectoryTree(const char* path) {
        if (!path || !path[0] || std::strlen(path) >= 384U) return false;
        char copy[384]{};
        std::snprintf(copy, sizeof(copy), "%s", path);
        for (char* cursor = copy + 1; *cursor; ++cursor) {
            if (*cursor != '/') continue;
            *cursor = '\0';
            if (copy[0] && mkdir(copy, 0775) != 0 && errno != EEXIST) {
                *cursor = '/';
                return false;
            }
            *cursor = '/';
        }
        return mkdir(copy, 0775) == 0 || errno == EEXIST;
    }

    static bool ensureParentDirectory(const char* path) {
        if (!path || std::strlen(path) >= 384U) return false;
        char parent[384]{};
        std::snprintf(parent, sizeof(parent), "%s", path);
        char* slash = std::strrchr(parent, '/');
        if (!slash) return true;
        *slash = '\0';
        return ensureDirectoryTree(parent);
    }

    bool dataPath(const char* requested, char* output, size_t capacity) const {
        char relative[192]{}, root[256]{};
        if (!normalizeGamePath(requested, relative, sizeof(relative)) ||
            !dataRoot(root, sizeof(root))) {
            return false;
        }
        const int length = relative[0]
            ? std::snprintf(output, capacity, "%s/%s", root, relative)
            : std::snprintf(output, capacity, "%s", root);
        return length > 0 && static_cast<size_t>(length) < capacity;
    }

    bool packagePath(const char* requested, char* output,
                     size_t capacity) const {
        if (!package_mode) return false;
        char relative[192]{};
        if (!normalizeGamePath(requested, relative, sizeof(relative))) return false;
        if (!relative[0]) {
            const int length = std::snprintf(output, capacity, "%s",
                                             package_info.path);
            return length > 0 && static_cast<size_t>(length) < capacity;
        }
        return pdxJoinPath(output, capacity, package_info.path, relative);
    }

    bool mergedStat(const char* requested, struct stat& value,
                    char* resolved = nullptr, size_t resolved_capacity = 0) const {
        char path[384]{};
        if (dataPath(requested, path, sizeof(path)) && stat(path, &value) == 0) {
            if (resolved && resolved_capacity) {
                std::snprintf(resolved, resolved_capacity, "%s", path);
            }
            return true;
        }
        if (packagePath(requested, path, sizeof(path)) && stat(path, &value) == 0) {
            if (resolved && resolved_capacity) {
                std::snprintf(resolved, resolved_capacity, "%s", path);
            }
            return true;
        }
        return false;
    }

    bool readMergedFile(const char* requested, std::string& bytes,
                        std::string& error) const {
        struct stat value{};
        char path[384]{};
        if (!mergedStat(requested, value, path, sizeof(path)) ||
            !S_ISREG(value.st_mode)) {
            error = "file not found";
            return false;
        }
        if (value.st_size < 0 || value.st_size > 2 * 1024 * 1024) {
            error = "file is too large";
            return false;
        }
        FILE* file = std::fopen(path, "rb");
        if (!file) {
            error = errno ? std::strerror(errno) : "open failed";
            return false;
        }
        bytes.resize(static_cast<size_t>(value.st_size));
        const bool ok = bytes.empty() ||
            std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
        std::fclose(file);
        if (!ok) {
            bytes.clear();
            error = errno ? std::strerror(errno) : "short read";
        }
        return ok;
    }

    static int pushJsonResult(lua_State* state, const char* bytes, size_t size) {
        std::string error;
        JsonToLua parser(state, bytes, size);
        if (parser.parse(error)) return 1;
        lua_pushnil(state);
        lua_pushlstring(state, error.data(), error.size());
        return 2;
    }

    static int cJsonDecode(lua_State* state) {
        size_t size = 0;
        const char* bytes = luaL_checklstring(state, 1, &size);
        return pushJsonResult(state, bytes, size);
    }

    static int cJsonDecodeFile(lua_State* state) {
        Impl* runtime = self(state);
        if (auto* input = static_cast<PdFile*>(
                luaL_testudata(state, 1, kFileMetatable))) {
            if (!input->handle) {
                lua_pushnil(state);
                lua_pushliteral(state, "file is closed");
                return 2;
            }
            const long original = std::ftell(input->handle);
            if (original < 0 || std::fseek(input->handle, 0, SEEK_END) != 0) {
                lua_pushnil(state);
                lua_pushliteral(state, "could not seek JSON file");
                return 2;
            }
            const long length = std::ftell(input->handle);
            if (length < 0 || length > 2 * 1024 * 1024 ||
                std::fseek(input->handle, 0, SEEK_SET) != 0) {
                if (original >= 0) std::fseek(input->handle, original, SEEK_SET);
                lua_pushnil(state);
                lua_pushliteral(state, "JSON file is too large or unreadable");
                return 2;
            }
            std::string bytes(static_cast<size_t>(length), '\0');
            const bool ok = bytes.empty() ||
                std::fread(bytes.data(), 1, bytes.size(), input->handle) == bytes.size();
            if (original >= 0) std::fseek(input->handle, original, SEEK_SET);
            if (!ok) {
                lua_pushnil(state);
                lua_pushliteral(state, "could not read JSON file");
                return 2;
            }
            return pushJsonResult(state, bytes.data(), bytes.size());
        }
        const char* requested = luaL_checkstring(state, 1);
        std::string bytes;
        std::string error;
        if (!runtime->readMergedFile(requested, bytes, error)) {
            lua_pushnil(state);
            lua_pushlstring(state, error.data(), error.size());
            return 2;
        }
        return pushJsonResult(state, bytes.data(), bytes.size());
    }

    static int pushFileError(lua_State* state, const char* fallback) {
        lua_pushnil(state);
        lua_pushstring(state, errno ? std::strerror(errno) : fallback);
        return 2;
    }

    static PdFile* checkedFile(lua_State* state) {
        return static_cast<PdFile*>(luaL_checkudata(state, 1, kFileMetatable));
    }

    static int cFileGc(lua_State* state) {
        auto* file = static_cast<PdFile*>(luaL_testudata(state, 1, kFileMetatable));
        if (file && file->handle) {
            std::fclose(file->handle);
            file->handle = nullptr;
        }
        return 0;
    }

    static int cFileClose(lua_State* state) {
        PdFile* file = checkedFile(state);
        if (file->handle) {
            std::fclose(file->handle);
            file->handle = nullptr;
        }
        return 0;
    }

    static int cFileWrite(lua_State* state) {
        PdFile* file = checkedFile(state);
        size_t length = 0;
        const char* bytes = luaL_checklstring(state, 2, &length);
        if (!file->handle || !file->writable) {
            lua_pushinteger(state, 0);
            lua_pushliteral(state, "file is not open for writing");
            return 2;
        }
        errno = 0;
        const size_t written = std::fwrite(bytes, 1, length, file->handle);
        lua_pushinteger(state, static_cast<lua_Integer>(written));
        if (written == length) return 1;
        lua_pushstring(state, errno ? std::strerror(errno) : "short write");
        return 2;
    }

    static int cFileFlush(lua_State* state) {
        PdFile* file = checkedFile(state);
        if (!file->handle) return pushFileError(state, "file is closed");
        errno = 0;
        if (std::fflush(file->handle) != 0) {
            return pushFileError(state, "flush failed");
        }
        return 0;
    }

    static int cFileRead(lua_State* state) {
        PdFile* file = checkedFile(state);
        const lua_Integer requested = luaL_checkinteger(state, 2);
        if (!file->handle) return pushFileError(state, "file is closed");
        if (requested < 0 || requested > 1024 * 1024) {
            return luaL_error(state, "read size must be between 0 and 1048576");
        }
        luaL_Buffer buffer;
        const size_t capacity = static_cast<size_t>(requested);
        char* output = luaL_buffinitsize(state, &buffer, capacity);
        errno = 0;
        const size_t got = capacity ? std::fread(output, 1, capacity, file->handle) : 0;
        if (got < capacity && std::ferror(file->handle)) {
            luaL_pushresultsize(&buffer, 0);
            lua_pop(state, 1);
            std::clearerr(file->handle);
            return pushFileError(state, "read failed");
        }
        luaL_pushresultsize(&buffer, got);
        lua_pushinteger(state, static_cast<lua_Integer>(got));
        return 2;
    }

    static int cFileReadline(lua_State* state) {
        PdFile* file = checkedFile(state);
        if (!file->handle) return pushFileError(state, "file is closed");
        luaL_Buffer buffer;
        luaL_buffinit(state, &buffer);
        size_t length = 0;
        errno = 0;
        for (;;) {
            const int value = std::fgetc(file->handle);
            if (value == EOF) break;
            if (value == '\n') break;
            if (value == '\r') {
                const int next = std::fgetc(file->handle);
                if (next != '\n' && next != EOF) std::ungetc(next, file->handle);
                break;
            }
            if (++length > 1024U * 1024U) {
                luaL_pushresult(&buffer);
                lua_pop(state, 1);
                return pushFileError(state, "line is too long");
            }
            luaL_addchar(&buffer, static_cast<char>(value));
        }
        if (length == 0 && std::feof(file->handle)) {
            luaL_pushresult(&buffer);
            lua_pop(state, 1);
            lua_pushnil(state);
            return 1;
        }
        if (std::ferror(file->handle)) {
            luaL_pushresult(&buffer);
            lua_pop(state, 1);
            std::clearerr(file->handle);
            return pushFileError(state, "readline failed");
        }
        luaL_pushresult(&buffer);
        return 1;
    }

    static int cFileSeek(lua_State* state) {
        PdFile* file = checkedFile(state);
        const long offset = static_cast<long>(luaL_checkinteger(state, 2));
        const int requested = static_cast<int>(luaL_optinteger(state, 3, 0));
        if (!file->handle) return pushFileError(state, "file is closed");
        const int origin = requested == 1 ? SEEK_CUR : (requested == 2 ? SEEK_END : SEEK_SET);
        errno = 0;
        if (std::fseek(file->handle, offset, origin) != 0) {
            return pushFileError(state, "seek failed");
        }
        lua_pushboolean(state, 1);
        return 1;
    }

    static int cFileTell(lua_State* state) {
        PdFile* file = checkedFile(state);
        if (!file->handle) return pushFileError(state, "file is closed");
        errno = 0;
        const long position = std::ftell(file->handle);
        if (position < 0) return pushFileError(state, "tell failed");
        lua_pushinteger(state, static_cast<lua_Integer>(position));
        return 1;
    }

    static int cFileOpen(lua_State* state) {
        Impl* runtime = self(state);
        const char* requested = luaL_checkstring(state, 1);
        const int mode = static_cast<int>(luaL_optinteger(state, 2, 0));
        if (!runtime->storage || !runtime->storage->mounted()) {
            errno = 0;
            return pushFileError(state, "SD card is not mounted");
        }
        char path[384]{};
        const char* fopen_mode = "rb";
        bool writable = false;
        if (mode == 1 || mode == 2) {
            writable = true;
            fopen_mode = mode == 1 ? "wb" : "ab";
            if (!runtime->dataPath(requested, path, sizeof(path)) ||
                !ensureParentDirectory(path)) {
                errno = 0;
                return pushFileError(state, "invalid or oversized file path");
            }
        } else {
            struct stat ignored{};
            if (!runtime->mergedStat(requested, ignored, path, sizeof(path)) ||
                S_ISDIR(ignored.st_mode)) {
                errno = ENOENT;
                return pushFileError(state, "file not found");
            }
        }
        errno = 0;
        FILE* handle = std::fopen(path, fopen_mode);
        if (!handle) return pushFileError(state, "open failed");
        auto* file = static_cast<PdFile*>(
            lua_newuserdatauv(state, sizeof(PdFile), 0));
        new (file) PdFile{handle, writable};
        luaL_getmetatable(state, kFileMetatable);
        lua_setmetatable(state, -2);
        return 1;
    }

    static int pushPdzLoadError(lua_State* state, const char* message) {
        lua_pushnil(state);
        lua_pushstring(state, message && message[0]
            ? message : "could not load PDZ file");
        return 2;
    }

    int pushPdzFunction(lua_State* state, const char* requested,
                        int environment_index) {
        int environment = 0;
        if (environment_index != 0 &&
            !lua_isnoneornil(state, environment_index)) {
            luaL_checktype(state, environment_index, LUA_TTABLE);
            environment = lua_absindex(state, environment_index);
        }
        char relative[192]{};
        if (!normalizeGamePath(requested, relative, sizeof(relative)) ||
            !relative[0]) {
            return pushPdzLoadError(state, "invalid PDZ path");
        }
        size_t relative_length = std::strlen(relative);
        if (relative_length < 4U ||
            std::strcmp(relative + relative_length - 4U, ".pdz") != 0) {
            if (relative_length + 4U >= sizeof(relative)) {
                return pushPdzLoadError(state, "PDZ path is too long");
            }
            std::memcpy(relative + relative_length, ".pdz", 5U);
        }

        struct stat value{};
        char path[384]{};
        if (!mergedStat(relative, value, path, sizeof(path)) ||
            !S_ISREG(value.st_mode)) {
            return pushPdzLoadError(state, "PDZ file not found");
        }

        char archive_error[128]{};
        external_pdz.close();
        const esp_err_t open_result = external_pdz.open(
            path, archive_error, sizeof(archive_error), false);
        if (open_result != ESP_OK) {
            external_pdz.close();
            return pushPdzLoadError(state, archive_error);
        }

        // pdc names a standalone source chunk after the file (data.pdz has a
        // Lua record named "data").  Accept the only Lua record as a fallback
        // because generated tools are permitted to choose another chunk name.
        char module[128]{};
        const char* basename = std::strrchr(relative, '/');
        basename = basename ? basename + 1 : relative;
        const size_t basename_length = std::strlen(basename);
        const size_t module_length = basename_length >= 4U
            ? basename_length - 4U : basename_length;
        if (module_length == 0U || module_length >= sizeof(module)) {
            external_pdz.close();
            return pushPdzLoadError(state, "invalid PDZ module name");
        }
        std::memcpy(module, basename, module_length);
        module[module_length] = '\0';
        const PdzEntry* entry = external_pdz.findLua(module);
        if (!entry && external_pdz.luaCount() == 1U) {
            for (size_t index = 0; index < external_pdz.count(); ++index) {
                const PdzEntry* candidate = external_pdz.entry(index);
                if (candidate && candidate->type == 1U) {
                    entry = candidate;
                    break;
                }
            }
        }
        if (!entry) {
            external_pdz.close();
            return pushPdzLoadError(state, "PDZ has no matching Lua chunk");
        }

        uint8_t* bytecode = nullptr;
        size_t bytecode_size = 0;
        const esp_err_t load_result = external_pdz.load(
            *entry, bytecode, bytecode_size, archive_error,
            sizeof(archive_error));
        if (load_result != ESP_OK) {
            external_pdz.close();
            return pushPdzLoadError(state, archive_error);
        }
        const bool normalized = normalizePlaydateLuaBytecode(
            bytecode, bytecode_size, archive_error, sizeof(archive_error));
        if (!normalized) {
            heap_caps_free(bytecode);
            external_pdz.close();
            return pushPdzLoadError(state, archive_error);
        }

        const int load_status = luaL_loadbufferx(
            state, reinterpret_cast<const char*>(bytecode), bytecode_size,
            relative, "b");
        heap_caps_free(bytecode);
        external_pdz.close();
        if (load_status != LUA_OK) {
            const char* message = lua_tostring(state, -1);
            lua_pushnil(state);
            lua_insert(state, -2);
            if (!message) {
                lua_pop(state, 1);
                lua_pushliteral(state, "invalid Lua bytecode in PDZ");
            }
            return 2;
        }

        if (environment != 0) {
            const int function_index = lua_absindex(state, -1);
            lua_pushvalue(state, environment);
            if (!lua_setupvalue(state, function_index, 1)) {
                // A valid Lua source chunk normally owns an _ENV upvalue.  If
                // a generated chunk does not, discard the unused value.
                lua_pop(state, 1);
            }
        }
        return 1;
    }

    static int cFileLoad(lua_State* state) {
        Impl* runtime = self(state);
        const char* requested = luaL_checkstring(state, 1);
        return runtime->pushPdzFunction(state, requested, 2);
    }

    static int cFileRun(lua_State* state) {
        const int loaded = cFileLoad(state);
        if (loaded != 1) return loaded;
        // cFileLoad() leaves the requested path/environment below the loaded
        // function because it is called directly here rather than through the
        // Lua VM. Move the function to slot one before executing it so only
        // values returned by the PDZ chunk are returned to the caller.
        lua_replace(state, 1);
        lua_settop(state, 1);
        if (lua_pcall(state, 0, LUA_MULTRET, 0) != LUA_OK) {
            const char* message = lua_tostring(state, -1);
            lua_pushnil(state);
            lua_insert(state, -2);
            if (!message) {
                lua_pop(state, 1);
                lua_pushliteral(state, "PDZ execution failed");
            }
            return 2;
        }
        return lua_gettop(state);
    }

    static bool appendDirectory(lua_State* state, const char* path,
                                bool show_hidden, int result, int seen,
                                size_t& count) {
        DIR* directory = opendir(path);
        if (!directory) return false;
        while (count < 256U) {
            dirent* entry = readdir(directory);
            if (!entry) break;
            if (!std::strcmp(entry->d_name, ".") ||
                !std::strcmp(entry->d_name, "..") ||
                (!show_hidden && entry->d_name[0] == '.')) {
                continue;
            }
            lua_getfield(state, seen, entry->d_name);
            const bool duplicate = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            if (duplicate) continue;
            char child[384]{};
            const int child_length = std::snprintf(child, sizeof(child), "%s/%s",
                                                   path, entry->d_name);
            if (child_length <= 0 || static_cast<size_t>(child_length) >= sizeof(child)) {
                continue;
            }
            struct stat value{};
            const bool directory_entry = stat(child, &value) == 0 &&
                                         S_ISDIR(value.st_mode);
            lua_pushboolean(state, 1);
            lua_setfield(state, seen, entry->d_name);
            if (directory_entry) {
                char name[272]{};
                std::snprintf(name, sizeof(name), "%s/", entry->d_name);
                lua_pushstring(state, name);
            } else {
                lua_pushstring(state, entry->d_name);
            }
            lua_rawseti(state, result, static_cast<lua_Integer>(++count));
        }
        closedir(directory);
        return true;
    }

    static int cFileList(lua_State* state) {
        Impl* runtime = self(state);
        const char* requested = luaL_optstring(state, 1, "");
        const bool show_hidden = lua_toboolean(state, 2) != 0;
        char relative[192]{};
        if (!normalizeGamePath(requested, relative, sizeof(relative))) {
            return luaL_error(state, "invalid file path");
        }
        lua_newtable(state);
        const int result = lua_absindex(state, -1);
        lua_newtable(state);
        const int seen = lua_absindex(state, -1);
        size_t count = 0;
        char path[384]{};
        if (runtime->dataPath(relative, path, sizeof(path))) {
            appendDirectory(state, path, show_hidden, result, seen, count);
        }
        if (runtime->packagePath(relative, path, sizeof(path))) {
            appendDirectory(state, path, show_hidden, result, seen, count);
        }
        lua_remove(state, seen);
        lua_getglobal(state, "table");
        lua_getfield(state, -1, "sort");
        lua_pushvalue(state, result);
        if (lua_pcall(state, 1, 0, 0) != LUA_OK) lua_pop(state, 1);
        lua_pop(state, 1);
        return 1;
    }

    static int cFileExists(lua_State* state) {
        struct stat value{};
        lua_pushboolean(state, self(state)->mergedStat(
            luaL_checkstring(state, 1), value));
        return 1;
    }

    static int cFileIsDir(lua_State* state) {
        struct stat value{};
        const bool exists = self(state)->mergedStat(luaL_checkstring(state, 1), value);
        lua_pushboolean(state, exists && S_ISDIR(value.st_mode));
        return 1;
    }

    static int cFileGetSize(lua_State* state) {
        struct stat value{};
        if (!self(state)->mergedStat(luaL_checkstring(state, 1), value)) {
            lua_pushnil(state);
            return 1;
        }
        lua_pushinteger(state, static_cast<lua_Integer>(value.st_size));
        return 1;
    }

    static int cFileGetType(lua_State* state) {
        struct stat value{};
        if (!self(state)->mergedStat(luaL_checkstring(state, 1), value)) {
            lua_pushnil(state);
            return 1;
        }
        lua_pushstring(state, S_ISDIR(value.st_mode) ? "directory" : "file");
        return 1;
    }

    static int cFileModtime(lua_State* state) {
        struct stat value{};
        if (!self(state)->mergedStat(luaL_checkstring(state, 1), value)) {
            lua_pushnil(state);
            return 1;
        }
        const std::tm* time = std::localtime(&value.st_mtime);
        if (!time) {
            lua_pushnil(state);
            return 1;
        }
        lua_newtable(state);
        auto set = [&](const char* name, int number) {
            lua_pushinteger(state, number);
            lua_setfield(state, -2, name);
        };
        set("year", time->tm_year + 1900);
        set("month", time->tm_mon + 1);
        set("day", time->tm_mday);
        set("hour", time->tm_hour);
        set("minute", time->tm_min);
        set("second", time->tm_sec);
        return 1;
    }

    static int cFileMkdir(lua_State* state) {
        Impl* runtime = self(state);
        char path[384]{};
        if (!runtime->dataPath(luaL_checkstring(state, 1), path, sizeof(path))) {
            lua_pushboolean(state, 0);
            return 1;
        }
        lua_pushboolean(state, ensureDirectoryTree(path));
        return 1;
    }

    static bool removeTree(const char* path, bool recursive, int depth = 0) {
        // The UI task has an 8 KiB stack. Keep recursive FAT traversal shallow
        // enough that a malicious package cannot turn delete() into a reboot.
        if (!path || depth > 6) return false;
        struct stat value{};
        if (stat(path, &value) != 0) return false;
        if (!S_ISDIR(value.st_mode)) return std::remove(path) == 0;
        if (!recursive) return std::remove(path) == 0;
        DIR* directory = opendir(path);
        if (!directory) return false;
        bool ok = true;
        while (dirent* entry = readdir(directory)) {
            if (!std::strcmp(entry->d_name, ".") ||
                !std::strcmp(entry->d_name, "..")) continue;
            char child[384]{};
            const int length = std::snprintf(child, sizeof(child), "%s/%s",
                                             path, entry->d_name);
            if (length <= 0 || static_cast<size_t>(length) >= sizeof(child) ||
                !removeTree(child, true, depth + 1)) {
                ok = false;
                break;
            }
        }
        closedir(directory);
        return ok && std::remove(path) == 0;
    }

    static int cFileDelete(lua_State* state) {
        Impl* runtime = self(state);
        const char* requested = luaL_checkstring(state, 1);
        const bool recursive = lua_toboolean(state, 2) != 0;
        char relative[192]{}, path[384]{};
        if (!normalizeGamePath(requested, relative, sizeof(relative)) ||
            !relative[0] || !runtime->dataPath(relative, path, sizeof(path))) {
            lua_pushboolean(state, 0);
            return 1;
        }
        lua_pushboolean(state, removeTree(path, recursive));
        return 1;
    }

    static int cFileRename(lua_State* state) {
        Impl* runtime = self(state);
        char from_relative[192]{}, to_relative[192]{};
        char from[384]{}, to[384]{};
        const bool valid = normalizeGamePath(luaL_checkstring(state, 1),
                                             from_relative, sizeof(from_relative)) &&
                           normalizeGamePath(luaL_checkstring(state, 2),
                                             to_relative, sizeof(to_relative)) &&
                           from_relative[0] && to_relative[0] &&
                           runtime->dataPath(from_relative, from, sizeof(from)) &&
                           runtime->dataPath(to_relative, to, sizeof(to));
        lua_pushboolean(state, valid && std::rename(from, to) == 0);
        return 1;
    }

    void savePath(char* path, size_t capacity, const char* name, const char* extension) const {
        const char* mount = storage ? storage->mountPoint() : "/sdcard";
        const char* prefix = game == Game::Celeste ? "celeste" :
                             (game == Game::PDSnake ? "pdsnake" : "external");
        char safe[48]{};
        size_t out = 0;
        for (const char* value = name && name[0] ? name : "data"; *value && out + 1 < sizeof(safe); ++value) {
            const char c = *value;
            safe[out++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_') ? c : '_';
        }
        char root[256]{};
        if (package_mode && dataRoot(root, sizeof(root))) {
            std::snprintf(path, capacity, "%s/%s.%s", root, safe, extension);
        } else {
            std::snprintf(path, capacity, "%s/pogodate/%s_%s.%s", mount,
                          prefix, safe, extension);
        }
    }

    bool writeValue(FILE* file, int index, int depth) {
        if (depth > 8) { std::fputs("nil", file); return true; }
        index = lua_absindex(lua, index);
        switch (lua_type(lua, index)) {
            case LUA_TNIL: std::fputs("nil", file); return true;
            case LUA_TBOOLEAN: std::fputs(lua_toboolean(lua,index) ? "true" : "false", file); return true;
            case LUA_TNUMBER:
                if (lua_isinteger(lua,index)) std::fprintf(file, "%lld", static_cast<long long>(lua_tointeger(lua,index)));
                else std::fprintf(file, "%.17g", lua_tonumber(lua,index));
                return true;
            case LUA_TSTRING: {
                size_t length=0; const char* value=lua_tolstring(lua,index,&length); std::fputc('"',file);
                for (size_t i=0;i<length;++i) {
                    const unsigned char c=value[i];
                    if (c=='"' || c=='\\') { std::fputc('\\',file); std::fputc(c,file); }
                    else if (c=='\n') std::fputs("\\n",file);
                    else if (c=='\r') std::fputs("\\r",file);
                    else if (c>=32) std::fputc(c,file);
                }
                std::fputc('"',file); return true;
            }
            case LUA_TTABLE: {
                std::fputc('{',file); bool first=true; lua_pushnil(lua);
                while (lua_next(lua,index)!=0) {
                    if (!first) std::fputc(',', file);
                    first = false;
                    std::fputc('[', file);
                    writeValue(file,-2,depth+1); std::fputs("]=",file); writeValue(file,-1,depth+1);
                    lua_pop(lua,1);
                }
                std::fputc('}',file); return true;
            }
            default: std::fputs("nil",file); return true;
        }
    }

    static int cDatastoreRead(lua_State* state) {
        // A few shipped Lua packages accidentally call read(table, name,
        // prettyPrint) at their save point.  Those arguments are the exact
        // datastore.write signature; accepting it keeps the save operation
        // recoverable while ordinary read(name) retains its documented form.
        if (lua_istable(state, 1)) return cDatastoreWrite(state);
        Impl* runtime=self(state); const char* name=luaL_optstring(state,1,"data");
        if (!runtime->storage || !runtime->storage->mounted()) { lua_pushnil(state); return 1; }
        char path[240]{}; runtime->savePath(path,sizeof(path),name,"lua");
        FILE* file=std::fopen(path,"rb"); if(!file){lua_pushnil(state);return 1;}
        std::fseek(file,0,SEEK_END); const long length=std::ftell(file); std::rewind(file);
        if(length<=0 || length>256*1024){std::fclose(file);lua_pushnil(state);return 1;}
        std::string data(static_cast<size_t>(length),'\0'); const size_t got=std::fread(data.data(),1,data.size(),file); std::fclose(file);
        if(got!=data.size() || luaL_loadbuffer(state,data.data(),data.size(),path)!=LUA_OK || lua_pcall(state,0,1,0)!=LUA_OK){
            if (lua_gettop(state) > 0) lua_pop(state, 1);
            lua_pushnil(state);
            return 1;
        }
        return 1;
    }

    static int cDatastoreWrite(lua_State* state) {
        Impl* runtime=self(state); luaL_checktype(state,1,LUA_TTABLE); const char* name=luaL_optstring(state,2,"data");
        if(!runtime->storage || !runtime->storage->mounted()){lua_pushboolean(state,0);return 1;}
        char path[240]{},temporary[240]{}; runtime->savePath(path,sizeof(path),name,"lua"); runtime->savePath(temporary,sizeof(temporary),name,"tmp");
        if (!ensureParentDirectory(temporary)) { lua_pushboolean(state,0);return 1; }
        FILE* file=std::fopen(temporary,"wb"); if(!file){lua_pushboolean(state,0);return 1;}
        std::fputs("return ",file); runtime->writeValue(file,1,0); std::fputc('\n',file);
        bool wrote=std::fclose(file)==0; if(wrote && std::rename(temporary,path)!=0){std::remove(path);wrote=std::rename(temporary,path)==0;}
        if (!wrote) std::remove(temporary);
        lua_pushboolean(state, wrote);
        return 1;
    }

    static int cDatastoreDelete(lua_State* state) {
        Impl* runtime=self(state); const char* name=luaL_optstring(state,1,"data");
        if(!runtime->storage || !runtime->storage->mounted()){lua_pushboolean(state,0);return 1;}
        char path[240]{}; runtime->savePath(path,sizeof(path),name,"lua");
        lua_pushboolean(state,std::remove(path)==0); return 1;
    }

    static int cGetSystemMenu(lua_State* state) {
        Impl* runtime=self(state); lua_newtable(state);
        runtime->setFunction(-1,"addCheckmarkMenuItem",cNoop);
        runtime->setFunction(-1,"addMenuItem",cNoop);
        runtime->setFunction(-1,"addOptionsMenuItem",cNoop);
        runtime->setFunction(-1,"removeMenuItem",cNoop);
        runtime->setFunction(-1,"removeAllMenuItems",cNoop);
        return 1;
    }

    void createMetatables() {
        if(luaL_newmetatable(lua,kFontMetatable)){
            lua_pushvalue(lua,-1);lua_setfield(lua,-2,"__index");
            setFunction(-1,"getHeight",cFontGetHeight);
            setFunction(-1,"getTextWidth",cFontGetTextWidth);
        } lua_pop(lua,1);
        if(luaL_newmetatable(lua,kImageMetatable)){
            lua_pushvalue(lua,-1);lua_setfield(lua,-2,"__index");
            pushFunction(cImageGc);lua_setfield(lua,-2,"__gc");
            setFunction(-1,"draw",cImageDraw);setFunction(-1,"drawCentered",cImageDrawCentered);
            setFunction(-1,"drawIgnoringOffset",cImageDrawIgnoringOffset);
            setFunction(-1,"drawScaled",cImageDrawScaled);
            setFunction(-1,"scaledImage",cImageScaledImage);
            setFunction(-1,"drawRotated",cImageDrawRotated);
            setFunction(-1,"drawFaded",cImageDrawFaded);setFunction(-1,"getSize",cImageGetSize);
            setFunction(-1,"sample",cImageSample);
            setFunction(-1,"setMaskImage",cImageSetMaskImage);
            setFunction(-1,"getMaskImage",cImageGetMaskImage);
            setFunction(-1,"addMask",cImageAddMask);
            setFunction(-1,"removeMask",cImageRemoveMask);
            setFunction(-1,"hasMask",cImageHasMask);
            setFunction(-1,"clearMask",cImageClearMask);
            setFunction(-1,"clear",cImageClear);setFunction(-1,"copy",cImageCopy);
            setFunction(-1,"setInverted",cImageSetInverted);
            setFunction(-1,"invertedImage",cImageInverted);setFunction(-1,"fadedImage",cImageFaded);
        } lua_pop(lua,1);
        if(luaL_newmetatable(lua,kImageTableMetatable)){
            pushFunction(cImageTableIndex);lua_setfield(lua,-2,"__index");
            setFunction(-1,"getImage",cImageTableGetImage);
            setFunction(-1,"drawImage",cImageTableDrawImage);
            setFunction(-1,"getLength",cImageTableGetLength);
            setFunction(-1,"getSize",cImageTableGetSize);
            pushFunction(cImageTableLen);lua_setfield(lua,-2,"__len");
        } lua_pop(lua,1);
        if(luaL_newmetatable(lua,kSoundMetatable)){
            lua_pushvalue(lua,-1);lua_setfield(lua,-2,"__index");
            setFunction(-1,"play",cSoundPlay);setFunction(-1,"stop",cSoundStop);
            setFunction(-1,"pause",cSoundPause);setFunction(-1,"isPlaying",cSoundIsPlaying);
            setFunction(-1,"setVolume",cSoundSetVolume);setFunction(-1,"setSample",cSoundSetSample);
            setFunction(-1,"setRate",cSoundSetRate);setFunction(-1,"getLength",cSoundGetLength);
            setFunction(-1,"setLoopRange",cNoop);
            setFunction(-1,"load",cSoundLoad);setFunction(-1,"setStopOnUnderrun",cNoop);
        } lua_pop(lua,1);
        if(luaL_newmetatable(lua,kSynthMetatable)){
            lua_pushvalue(lua,-1);lua_setfield(lua,-2,"__index");
            pushFunction(cSynthGc);lua_setfield(lua,-2,"__gc");
            setFunction(-1,"copy",cSynthCopy);
            setFunction(-1,"playNote",cSynthPlayNote);
            setFunction(-1,"playMIDINote",cSynthPlayMidiNote);
            setFunction(-1,"noteOff",cSynthNoteOff);
            setFunction(-1,"stop",cSynthStop);
            setFunction(-1,"isPlaying",cSynthIsPlaying);
            setFunction(-1,"setWaveform",cSynthSetWaveform);
            setFunction(-1,"setSample",cSynthSetWaveform);
            setFunction(-1,"setWavetable",cSynthSetWavetable);
            setFunction(-1,"setADSR",cSynthSetAdsr);
            setFunction(-1,"setAttack",cSynthSetAttack);
            setFunction(-1,"setDecay",cSynthSetDecay);
            setFunction(-1,"setSustain",cSynthSetSustain);
            setFunction(-1,"setRelease",cSynthSetRelease);
            setFunction(-1,"clearEnvelope",cSynthClearEnvelope);
            setFunction(-1,"getEnvelope",cSynthGetEnvelope);
            setFunction(-1,"setTranspose",cSynthSetTranspose);
            setFunction(-1,"setVolume",cSynthSetVolume);
            setFunction(-1,"getVolume",cSynthGetVolume);
            setFunction(-1,"setParameter",cSynthSetParameter);
            setFunction(-1,"getParameterCount",cSynthGetParameterCount);
            setFunction(-1,"setParameterMod",cNoop);
            setFunction(-1,"setFrequencyMod",cNoop);
            setFunction(-1,"setFrequencyModulator",cNoop);
            setFunction(-1,"setAmplitudeMod",cNoop);
            setFunction(-1,"setAmplitudeModulator",cNoop);
            setFunction(-1,"setLegato",cNoop);
            setFunction(-1,"setFinishCallback",cNoop);
            setFunction(-1,"setEnvelopeCurvature",cNoop);
            setFunction(-1,"setCurvature",cNoop);
            setFunction(-1,"setVelocitySensitivity",cNoop);
            setFunction(-1,"setRateScaling",cNoop);
            setFunction(-1,"setScale",cNoop);
        } lua_pop(lua,1);
        if(luaL_newmetatable(lua,kFileMetatable)){
            lua_pushvalue(lua,-1);lua_setfield(lua,-2,"__index");
            lua_pushcfunction(lua,cFileGc);lua_setfield(lua,-2,"__gc");
            setFunction(-1,"close",cFileClose);setFunction(-1,"write",cFileWrite);
            setFunction(-1,"flush",cFileFlush);setFunction(-1,"read",cFileRead);
            setFunction(-1,"readline",cFileReadline);setFunction(-1,"seek",cFileSeek);
            setFunction(-1,"tell",cFileTell);
        } lua_pop(lua,1);
    }

    void createTimerRegistry() {
        lua_newtable(lua);lua_rawsetp(lua,LUA_REGISTRYINDEX,&kTimerRegistryKey);
    }

    void setInteger(int table, const char* name, lua_Integer value) {
        table = lua_absindex(lua, table);
        lua_pushinteger(lua, value);
        lua_setfield(lua, table, name);
    }
    void setBoolean(int table, const char* name, bool value) {
        table = lua_absindex(lua, table);
        lua_pushboolean(lua, value);
        lua_setfield(lua, table, name);
    }

    void registerApi() {
        createMetatables();createTimerRegistry();
        pushFunction(cImport);lua_setglobal(lua,"import");
        lua_newtable(lua);const int playdate=lua_gettop(lua);
        setInteger(playdate,"kButtonLeft",0x04);setInteger(playdate,"kButtonRight",0x08);
        setInteger(playdate,"kButtonUp",0x01);setInteger(playdate,"kButtonDown",0x02);
        setInteger(playdate,"kButtonA",0x20);setInteger(playdate,"kButtonB",0x10);
        setBoolean(playdate,"isSimulator",false);
        setFunction(playdate,"getCurrentTimeMilliseconds",cGetCurrentTime);
        setFunction(playdate,"getElapsedTime",cGetElapsedTime);
        setFunction(playdate,"resetElapsedTime",cResetElapsedTime);
        setFunction(playdate,"getFPS",cGetRefreshRate);
        setFunction(playdate,"buttonJustPressed",cButtonJustPressed);setFunction(playdate,"buttonIsPressed",cButtonIsPressed);
        setFunction(playdate,"buttonJustReleased",cButtonJustReleased);setFunction(playdate,"getButtonState",cGetButtonState);
        setFunction(playdate,"getSystemMenu",cGetSystemMenu);setFunction(playdate,"setMenuImage",cSetMenuImage);
        setFunction(playdate,"drawFPS",cDrawFps);setFunction(playdate,"getSecondsSinceEpoch",cGetSecondsSinceEpoch);
        setFunction(playdate,"getTime",cGetTime);setFunction(playdate,"getGMTTime",cGetGmtTime);
        setFunction(playdate,"epochFromTime",cEpochFromTime);
        setFunction(playdate,"epochFromGMTTime",cEpochFromGmtTime);
        setFunction(playdate,"timeFromEpoch",cTimeFromEpoch);
        setFunction(playdate,"GMTTimeFromEpoch",cGmtTimeFromEpoch);
        setFunction(playdate,"getReduceFlashing",cGetReduceFlashing);
        setFunction(playdate,"isCrankDocked",cIsCrankDocked);
        setFunction(playdate,"getCrankPosition",cGetCrankPosition);
        setFunction(playdate,"getCrankChange",cGetCrankChange);
        setFunction(playdate,"getCrankTicks",cGetCrankTicks);
        setFunction(playdate,"setCrankSoundsDisabled",cSetCrankSoundsDisabled);
        setFunction(playdate,"setAutoLockDisabled",cNoop);
        setFunction(playdate,"startAccelerometer",cStartAccelerometer);
        setFunction(playdate,"stopAccelerometer",cStopAccelerometer);
        setFunction(playdate,"accelerometerIsRunning",cAccelerometerIsRunning);
        setFunction(playdate,"readAccelerometer",cReadAccelerometer);
        lua_newtable(lua);
        const char* metadata_name = package_mode && package_info.name[0]
            ? package_info.name : (game==Game::Celeste?"Celeste Classic":"PDSnake");
        const char* metadata_version = package_mode && package_info.version[0]
            ? package_info.version : (game==Game::Celeste?"1.0.3":"1.2");
        const char* metadata_author = package_mode && package_info.author[0]
            ? package_info.author : (game==Game::Celeste?"HTeuMeuLeu":"Brett Chalupa");
        lua_pushstring(lua,metadata_name);lua_setfield(lua,-2,"name");
        lua_pushstring(lua,metadata_version);lua_setfield(lua,-2,"version");
        lua_pushstring(lua,game==Game::Celeste?"8":"0");lua_setfield(lua,-2,"buildNumber");
        lua_pushstring(lua,metadata_author);lua_setfield(lua,-2,"author");
        lua_pushstring(lua,package_mode && package_info.bundle_id[0]
            ? package_info.bundle_id : (game==Game::Celeste
                ? "com.hteumeuleu.celeste" : "com.pogopo.pdsnake"));
        lua_setfield(lua,-2,"bundleID");
        lua_setfield(lua,playdate,"metadata");

        lua_newtable(lua);setFunction(-1,"getWidth",cDisplayWidth);setFunction(-1,"getHeight",cDisplayHeight);
        setFunction(-1,"getSize",cDisplaySize);
        setFunction(-1,"setRefreshRate",cSetRefreshRate);setFunction(-1,"getRefreshRate",cGetRefreshRate);
        setFunction(-1,"flush",cNoop);setFunction(-1,"setScale",cSetScale);
        setFunction(-1,"setOffset",cSetOffset);setFunction(-1,"setInverted",cSetInverted);
        lua_setfield(lua,playdate,"display");

        lua_newtable(lua);const int graphics=lua_gettop(lua);
        setInteger(graphics,"kColorBlack",PdColorBlack);
        setInteger(graphics,"kColorWhite",PdColorWhite);
        setInteger(graphics,"kColorClear",PdColorClear);
        setInteger(graphics,"kColorXOR",PdColorXor);
        setInteger(graphics,"kDrawModeCopy",Copy);
        setInteger(graphics,"kDrawModeWhiteTransparent",WhiteTransparent);
        setInteger(graphics,"kDrawModeBlackTransparent",BlackTransparent);
        setInteger(graphics,"kDrawModeFillWhite",FillWhite);
        setInteger(graphics,"kDrawModeFillBlack",FillBlack);
        setInteger(graphics,"kDrawModeXOR",DrawXor);
        setInteger(graphics,"kDrawModeNXOR",Nxor);
        setInteger(graphics,"kDrawModeInverted",Inverted);
        setInteger(graphics,"kImageUnflipped",Unflipped);
        setInteger(graphics,"kImageFlippedX",FlippedX);setInteger(graphics,"kImageFlippedY",FlippedY);
        setInteger(graphics,"kImageFlippedXY",FlippedXY);setInteger(graphics,"kStrokeInside",0);setInteger(graphics,"kStrokeOutside",1);
        setFunction(graphics,"_beginFrame",cGraphicsBeginFrame);setFunction(graphics,"_getImageDrawMode",cGetDrawMode);
        setFunction(graphics,"_drawTilemap",cDrawTilemap);
        setFunction(graphics,"getImageDrawMode",cGetDrawMode);
        setFunction(graphics,"getDisplayImage",cGetDisplayImage);
        setFunction(graphics,"getWorkingImage",cGetDisplayImage);
        setFunction(graphics,"clear",cGraphicsClear);setFunction(graphics,"setColor",cSetColor);
        setFunction(graphics,"setPattern",cSetPattern);
        setFunction(graphics,"setDitherPattern",cSetDitherPattern);
        setFunction(graphics,"setDrawOffset",cSetDrawOffset);
        setFunction(graphics,"getDrawOffset",cGetDrawOffset);
        setFunction(graphics,"setImageDrawMode",cSetDrawMode);setFunction(graphics,"setLineWidth",cSetLineWidth);
        setFunction(graphics,"setStrokeLocation",cSetStrokeLocation);setFunction(graphics,"fillRect",cFillRect);
        setFunction(graphics,"drawRect",cDrawRect);
        setFunction(graphics,"fillRoundRect",cFillRoundRect);
        setFunction(graphics,"drawRoundRect",cDrawRoundRect);
        setFunction(graphics,"drawLine",cDrawLine);
        setFunction(graphics,"fillCircleAtPoint",cFillCircle);setFunction(graphics,"drawCircleAtPoint",cDrawCircle);
        setFunction(graphics,"fillCircleInRect",cFillCircleInRect);setFunction(graphics,"drawCircleInRect",cDrawCircleInRect);
        setFunction(graphics,"drawText",cDrawText);setFunction(graphics,"drawTextInRect",cDrawTextInRect);
        setFunction(graphics,"drawTextAligned",cDrawTextAligned);
        setFunction(graphics,"getTextSize",cGetTextSize);
        setFunction(graphics,"setFont",cSetFont);setFunction(graphics,"getFont",cGetFont);
        setFunction(graphics,"setFontFamily",cSetFontFamily);
        setFunction(graphics,"getSystemFont",cGetSystemFont);
        setFunction(graphics,"pushContext",cPushContext);
        setFunction(graphics,"popContext",cPopContext);
        setFunction(graphics,"lockFocus",cLockFocus);
        setFunction(graphics,"unlockFocus",cUnlockFocus);
        setFunction(graphics,"setClipRect",cSetClipRect);
        setFunction(graphics,"getClipRect",cGetClipRect);
        setFunction(graphics,"clearClipRect",cClearClipRect);setFunction(graphics,"setStencilImage",cSetStencil);
        setFunction(graphics,"clearStencil",cClearStencil);setFunction(graphics,"setBackgroundColor",cSetBackgroundColor);
        lua_newtable(lua);setFunction(-1,"new",cFontNew);
        lua_pushliteral(lua,"normal");lua_setfield(lua,-2,"kVariantNormal");
        lua_pushliteral(lua,"bold");lua_setfield(lua,-2,"kVariantBold");
        lua_pushliteral(lua,"italic");lua_setfield(lua,-2,"kVariantItalic");
        lua_setfield(lua,graphics,"font");
        lua_newtable(lua);setFunction(-1,"new",cImageNew);
        setInteger(-1,"kDitherTypeNone",0);
        setInteger(-1,"kDitherTypeDiagonalLine",1);
        setInteger(-1,"kDitherTypeVerticalLine",2);
        setInteger(-1,"kDitherTypeHorizontalLine",3);
        setInteger(-1,"kDitherTypeScreen",4);
        setInteger(-1,"kDitherTypeBayer2x2",5);
        setInteger(-1,"kDitherTypeBayer4x4",6);
        setInteger(-1,"kDitherTypeBayer8x8",7);
        setInteger(-1,"kDitherTypeFloydSteinberg",8);
        setInteger(-1,"kDitherTypeBurkes",9);
        setInteger(-1,"kDitherTypeAtkinson",10);
        lua_setfield(lua,graphics,"image");
        lua_newtable(lua);setFunction(-1,"new",cImageTableNew);lua_setfield(lua,graphics,"imagetable");
        lua_setfield(lua,playdate,"graphics");

        lua_newtable(lua);const int sound=lua_gettop(lua);
        setInteger(sound,"kWaveSquare",0);
        setInteger(sound,"kWaveTriangle",1);
        setInteger(sound,"kWaveSine",2);
        setInteger(sound,"kWaveNoise",3);
        setInteger(sound,"kWaveSawtooth",4);
        setInteger(sound,"kWavePOPhase",5);
        setInteger(sound,"kWavePODigital",6);
        setInteger(sound,"kWavePOVosim",7);
        setFunction(sound,"getCurrentTime",cSoundGetCurrentTime);
        lua_newtable(lua);setFunction(-1,"new",cSoundNew);lua_setfield(lua,sound,"sample");
        lua_newtable(lua);setFunction(-1,"new",cSoundNew);lua_setfield(lua,sound,"sampleplayer");
        lua_newtable(lua);setFunction(-1,"new",cFilePlayerNew);lua_setfield(lua,sound,"fileplayer");
        lua_newtable(lua);setFunction(-1,"new",cSynthNew);lua_setfield(lua,sound,"synth");
        lua_setfield(lua,playdate,"sound");
        lua_newtable(lua);setFunction(-1,"new",cTimerNew);setFunction(-1,"performAfterDelay",cTimerAfter);
        setFunction(-1,"updateTimers",cUpdateTimers);setFunction(-1,"allTimers",cTimerAll);
        lua_setfield(lua,playdate,"timer");
        lua_newtable(lua);setFunction(-1,"read",cDatastoreRead);setFunction(-1,"write",cDatastoreWrite);
        setFunction(-1,"delete",cDatastoreDelete);lua_setfield(lua,playdate,"datastore");
        lua_newtable(lua);const int file=lua_gettop(lua);
        setInteger(file,"kFileRead",0);setInteger(file,"kFileWrite",1);
        setInteger(file,"kFileAppend",2);setInteger(file,"kSeekSet",0);
        setInteger(file,"kSeekFromCurrent",1);setInteger(file,"kSeekFromEnd",2);
        setFunction(file,"open",cFileOpen);setFunction(file,"load",cFileLoad);
        setFunction(file,"run",cFileRun);setFunction(file,"listFiles",cFileList);
        setFunction(file,"exists",cFileExists);setFunction(file,"isdir",cFileIsDir);
        setFunction(file,"mkdir",cFileMkdir);setFunction(file,"delete",cFileDelete);
        setFunction(file,"getSize",cFileGetSize);setFunction(file,"getType",cFileGetType);
        setFunction(file,"modtime",cFileModtime);setFunction(file,"rename",cFileRename);
        lua_newtable(lua);lua_setfield(lua,file,"file");
        lua_setfield(lua,playdate,"file");
        lua_setglobal(lua,"playdate");

        lua_newtable(lua);
        setFunction(-1,"decode",cJsonDecode);
        setFunction(-1,"decodeFile",cJsonDecodeFile);
        lua_setglobal(lua,"json");
    }

    bool callGlobal(const char* table_name,const char* function_name,bool optional=false) {
        const int top=lua_gettop(lua);lua_getglobal(lua,table_name);
        if(!lua_istable(lua,-1)){lua_settop(lua,top);if(!optional)setError(function_name,"missing table");return optional;}
        lua_getfield(lua,-1,function_name);
        if(!lua_isfunction(lua,-1)){lua_settop(lua,top);if(!optional)setError(function_name,"missing function");return optional;}
        if(lua_pcall(lua,0,0,0)!=LUA_OK){const bool result=takeLuaError(function_name);lua_settop(lua,top);return result;}
        lua_settop(lua,top);return true;
    }

    bool dispatchInput(uint8_t pressed,uint8_t released) {
        const int top=lua_gettop(lua);lua_getglobal(lua,"_pogodate_dispatch_input");
        if(!lua_isfunction(lua,-1)){lua_settop(lua,top);return true;}
        lua_pushinteger(lua,pressed);lua_pushinteger(lua,released);
        if(lua_pcall(lua,2,0,0)!=LUA_OK){const bool result=takeLuaError("input");lua_settop(lua,top);return result;}
        lua_settop(lua,top);return true;
    }

    bool dispatchCrank(bool& consumed) {
        consumed = false;
        if (crank_pending_change == 0.0f && crank_dock_event == 0) return true;
        const int top = lua_gettop(lua);
        lua_getglobal(lua, "_pogodate_dispatch_crank");
        if (!lua_isfunction(lua, -1)) {
            lua_settop(lua, top);
            return true;
        }
        lua_pushnumber(lua, crank_pending_change);
        lua_pushnumber(lua, crank_pending_accelerated_change);
        lua_pushinteger(lua, crank_dock_event);
        if (lua_pcall(lua, 3, 1, 0) != LUA_OK) {
            const bool result = takeLuaError("crank input");
            lua_settop(lua, top);
            return result;
        }
        consumed = lua_toboolean(lua, -1) != 0;
        crank_dock_event = 0;
        lua_settop(lua, top);
        return true;
    }

    esp_err_t start(gfx::Canvas& target_canvas,audio::Audio& target_audio,
                    storage::Storage& target_storage,Game selected_game,
                    const char* package_path = nullptr) {
        canvas=&target_canvas;audio=&target_audio;storage=&target_storage;game=selected_game;
        last_error[0]='\0';loaded_modules.fill(false);runtime_stats={};
        for (auto& name : loaded_external_modules) name.fill('\0');
        loaded_external_count=0;external_import_depth=0;
        allocated_bytes=0;peak_allocated_bytes=0;
        package_mode=false;package_info={};pdz.close();external_pdz.close();
        if (package_path && package_path[0]) {
            const esp_err_t inspect_error = inspectPackage(package_path, package_info);
            if (inspect_error != ESP_OK) {
                setError("PDX", "invalid or incomplete package");
                return inspect_error;
            }
            if (package_info.kind == PackageKind::NativeBinary) {
                setError("PDX", "pdex.bin is ARM code; ESP32-S3 requires Lua main.pdz");
                return ESP_ERR_NOT_SUPPORTED;
            }
            char pdz_path[224]{}, archive_error[128]{};
            if (!pdxJoinPath(pdz_path, sizeof(pdz_path), package_info.path, "main.pdz")) {
                setError("PDX", "main.pdz path is too long");
                return ESP_ERR_INVALID_SIZE;
            }
            const esp_err_t archive_result = pdz.open(
                pdz_path, archive_error, sizeof(archive_error));
            if (archive_result != ESP_OK) {
                setError("PDZ", archive_error);
                return archive_result;
            }
            package_mode = true;
            if (!std::strcmp(package_info.bundle_id, "com.hteumeuleu.celeste") ||
                std::strstr(package_info.name, "Celeste")) game = Game::Celeste;
            else game = Game::External;
            preloadShortSounds();
        }
        // Keep each game's original simulation speed.  The panel can still
        // present up to 50 Hz, but forcing a frame-based 30 FPS game to run
        // 50 update callbacks per second makes physics and animation 1.67x
        // faster.  Packages can request another rate with setRefreshRate().
        refresh_rate=30;runtime_stats.requested_fps=refresh_rate;
        frame_accumulator_units=0;now_ms=0;elapsed_reset_ms=0;
        held_buttons=pressed_buttons=previous_held_buttons=0;
        crank_position=0.0f;crank_unwrapped_position=0.0;
        crank_tick_previous_position=0.0;crank_pending_change=0.0f;
        crank_pending_accelerated_change=0.0f;crank_sample_ms=0;
        crank_initialized=false;crank_docked=false;crank_dock_event=0;
        crank_sounds_disabled=false;
        accelerometer_x=accelerometer_y=0.0f;accelerometer_z=1.0f;
        accelerometer_valid=false;accelerometer_running=false;
        next_timer_id=1;display_scale=1;display_offset_x=display_offset_y=0;
        draw_offset_x=draw_offset_y=0;
        inverted_display=false;background_color=White;
        draw_color=solid_draw_color=draw_pattern_color=Black;
        draw_pattern_transparent=false;draw_pattern.fill(0xffU);
        draw_pattern_pixels.fill(Clear);draw_pattern_uses_image=false;
        pattern_offset_x=pattern_offset_y=0;current_font=nullptr;
        current_font_ref=LUA_NOREF;system_font_ref=LUA_NOREF;
        maze_completion_image_ref=LUA_NOREF;
        maze_completion_reuse_logged=false;
        if(!resizeScreen(1)){clearSoundCache();setError("startup","screen buffer allocation failed");return ESP_ERR_NO_MEM;}
        resetTargetToScreen();
        lua=lua_newstate(allocator,this);if(!lua){releaseImage(screen);clearSoundCache();setError("startup","could not allocate Lua state");return ESP_ERR_NO_MEM;}
        luaL_openlibs(lua);registerApi();
        ESP_LOGI(TAG, "%s",
                 "PogoDate API STEP11.6.13: external PDZ load + run");
        size_t compat_size=0;const char* compat=compatSource(compat_size);
        if(!loadBuffer("PogoDate CoreLibs compatibility",compat,compat_size)){
            lua_close(lua);lua=nullptr;clearLargeImagePool();releaseImage(screen);clearSoundCache();return ESP_FAIL;
        }
        // Our native animation/animator replacements intentionally skip the
        // bundled CoreLibs modules, but the stock animation module normally
        // installs playdate.easingFunctions as a side effect.  Load the
        // package's exact pure-Lua easing implementation first when present;
        // older Noble/Sequence releases otherwise return early and leave
        // Sequence.new undefined even though their module itself loaded.
        if(package_mode && pdz.findLua("CoreLibs/easing") &&
           !importModule("CoreLibs/easing")){
            lua_close(lua);lua=nullptr;clearLargeImagePool();releaseImage(screen);clearSoundCache();return ESP_FAIL;
        }
        if(!importModule("main")){
            lua_close(lua);lua=nullptr;clearLargeImagePool();releaseImage(screen);clearSoundCache();return ESP_FAIL;
        }
        // Incremental collection trades rare long stop-the-world nursery/major
        // sweeps for small, regular slices.  A low pause keeps the Lua heap
        // close to its live size while the small step size avoids animation
        // spikes on PSRAM-backed allocations.
        lua_gc(lua, LUA_GCINC, 110, 200, 8);
        is_running=true;
        ESP_LOGI(TAG,"PogoDate Lite ready: %s %s Lua 5.4, 400x240, logic=%lu FPS LCD cap=50, screen=%s, audio cache=%lu",
                 package_mode ? package_info.name :
                    (game==Game::Celeste?"Celeste Classic 1.0.3":"PDSnake 1.2"),
                 package_mode ? "SD main.pdz" : "source",
                 static_cast<unsigned long>(refresh_rate),
                 screen_in_internal_ram ? "internal" : "PSRAM",
                 static_cast<unsigned long>(sound_cache_bytes));
        return ESP_OK;
    }

    void stop() {
        if(lua&&is_running)callGlobal("playdate","gameWillTerminate",true);
        if(audio)audio->stopMusicPcm();
        is_running=false;if(lua){lua_close(lua);lua=nullptr;}
        clearLargeImagePool();
        clearSoundCache();
        pdz.close();external_pdz.close();package_mode=false;package_info={};
        loaded_external_count=0;external_import_depth=0;
        releaseImage(screen);
        target=nullptr;stencil=nullptr;current_font=nullptr;
        current_font_ref=LUA_NOREF;system_font_ref=LUA_NOREF;
        maze_completion_image_ref=LUA_NOREF;
        maze_completion_reuse_logged=false;
        canvas=nullptr;audio=nullptr;storage=nullptr;
    }

    uint32_t update(uint32_t dt_ms) {
        if(!lua||!is_running)return 0;
        now_ms+=dt_ms;
        uint64_t accumulated = static_cast<uint64_t>(frame_accumulator_units) +
            static_cast<uint64_t>(dt_ms) * std::max<uint32_t>(1U, refresh_rate);
        // Preserve the sub-frame remainder but discard complete missed frames.
        // A 100 ms audio/GC stall must not turn into several rapid catch-up
        // updates that visibly speed up physics after the hitch.
        if (accumulated >= 2000U) accumulated = 1000U + accumulated % 1000U;
        frame_accumulator_units = static_cast<uint32_t>(accumulated);
        uint32_t produced=0;
        // Never run multiple slow Lua frames before returning to AppManager.
        // The old three-frame catch-up loop made a 5 FPS VM reach the LCD only
        // once per three updates, which is why the visible rate was ~1-2 FPS.
        if(frame_accumulator_units>=1000U){
            frame_accumulator_units-=1000U;
            frame_dt_ms=(1000U+refresh_rate/2U)/std::max<uint32_t>(1U,refresh_rate);
            const uint8_t released=static_cast<uint8_t>(previous_held_buttons&~held_buttons);
            const int64_t started=esp_timer_get_time();
            bool crank_consumed=false;
            if(!dispatchInput(pressed_buttons,released)||
               !dispatchCrank(crank_consumed)||!callGlobal("playdate","update")){
                is_running=false;
                return 0;
            }
            if(crank_consumed){
                crank_pending_change=0.0f;
                crank_pending_accelerated_change=0.0f;
            }
            const int64_t logic_finished=esp_timer_get_time();
            flushScreen();previous_held_buttons=held_buttons;
            const int64_t finished=esp_timer_get_time();
            const uint32_t elapsed=static_cast<uint32_t>(std::max<int64_t>(0,finished-started));
            runtime_stats.last_logic_us=static_cast<uint32_t>(std::max<int64_t>(0,logic_finished-started));
            runtime_stats.last_blit_us=static_cast<uint32_t>(std::max<int64_t>(0,finished-logic_finished));
            runtime_stats.last_update_us=elapsed;runtime_stats.max_update_us=std::max(runtime_stats.max_update_us,elapsed);
            ++runtime_stats.lua_frames;++produced;pressed_buttons=0;
        }
        runtime_stats.lua_bytes=allocated_bytes;runtime_stats.lua_peak_bytes=peak_allocated_bytes;
        if(lua)runtime_stats.lua_gc_bytes=static_cast<size_t>(lua_gc(lua,LUA_GCCOUNT))*1024U+
            static_cast<size_t>(lua_gc(lua,LUA_GCCOUNTB));
        return produced;
    }
};

Runtime::Runtime():impl_(new(std::nothrow)Impl){}
Runtime::~Runtime(){stop();delete impl_;impl_=nullptr;}
esp_err_t Runtime::start(gfx::Canvas& canvas,audio::Audio& audio,
                         storage::Storage& storage,Game game){
    if (!impl_) return ESP_ERR_NO_MEM;
    impl_->stop();
    return impl_->start(canvas, audio, storage, game);
}
esp_err_t Runtime::startPackage(gfx::Canvas& canvas,audio::Audio& audio,
                                storage::Storage& storage,const char* pdx_path){
    if (!impl_) return ESP_ERR_NO_MEM;
    impl_->stop();
    return impl_->start(canvas, audio, storage, Game::External, pdx_path);
}
void Runtime::stop(){if(impl_)impl_->stop();}
void Runtime::setInput(uint8_t held_mask,uint8_t pressed_mask){if(!impl_)return;impl_->held_buttons=held_mask;impl_->pressed_buttons=static_cast<uint8_t>(impl_->pressed_buttons|pressed_mask);}
void Runtime::setCrank(float position_degrees,bool docked,bool valid){
    if(!impl_)return;
    if(!valid){position_degrees=0.0f;docked=false;}
    if(!std::isfinite(position_degrees))position_degrees=0.0f;
    position_degrees=std::fmod(position_degrees,360.0f);
    if(position_degrees<0.0f)position_degrees+=360.0f;
    if(!impl_->crank_initialized){
        impl_->crank_position=position_degrees;
        impl_->crank_unwrapped_position=position_degrees;
        impl_->crank_tick_previous_position=position_degrees;
        impl_->crank_sample_ms=impl_->now_ms;
        impl_->crank_initialized=true;
    }else{
        float change=position_degrees-impl_->crank_position;
        if(change>180.0f)change-=360.0f;
        else if(change<=-180.0f)change+=360.0f;
        const uint32_t elapsed=std::max<uint32_t>(1U,impl_->now_ms-impl_->crank_sample_ms);
        const float degrees_per_second=std::fabs(change)*1000.0f/static_cast<float>(elapsed);
        const float acceleration=1.0f+std::min(4.0f,degrees_per_second/180.0f);
        impl_->crank_pending_change+=change;
        impl_->crank_pending_accelerated_change+=change*acceleration;
        impl_->crank_unwrapped_position+=change;
        impl_->crank_position=position_degrees;
        impl_->crank_sample_ms=impl_->now_ms;
    }
    if(impl_->crank_docked!=docked){
        impl_->crank_docked=docked;
        impl_->crank_dock_event=docked?1:-1;
    }
}
void Runtime::setAccelerometer(float x,float y,float z,bool valid){
    if(!impl_)return;
    impl_->accelerometer_x=x;impl_->accelerometer_y=y;impl_->accelerometer_z=z;
    impl_->accelerometer_valid=valid;
}
uint32_t Runtime::update(uint32_t dt_ms){return impl_?impl_->update(dt_ms):0;}
bool Runtime::running()const{return impl_&&impl_->is_running;}
const char* Runtime::error()const{return impl_&&impl_->last_error[0]?impl_->last_error:"";}
Stats Runtime::stats()const{return impl_?impl_->runtime_stats:Stats{};}
#ifdef PD_HOST_TEST
bool Runtime::evalForTest(const char* source){
    return impl_&&impl_->lua&&source&&
        impl_->loadBuffer("PogoDate host regression",source,std::strlen(source));
}
#endif

} // namespace pogopo::playdate
