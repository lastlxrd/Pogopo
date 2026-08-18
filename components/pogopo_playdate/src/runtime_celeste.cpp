#include "pogopo/playdate/runtime.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <new>
#include <string>
#include <sys/stat.h>

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
    FillWhite = 1,
    FillBlack = 2,
    Inverted = 3,
    Nxor = 4,
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
    bool package_mode = false;

    bool is_running = false;
    bool inverted_display = false;
    uint8_t draw_color = Black;
    std::array<uint8_t, 8> draw_pattern{{0xff, 0xff, 0xff, 0xff,
                                         0xff, 0xff, 0xff, 0xff}};
    int pattern_offset_x = 0;
    int pattern_offset_y = 0;
    uint8_t background_color = White;
    int draw_mode = Copy;
    int line_width = 1;
    int stroke_location = 0;
    int display_scale = 1;
    int display_offset_x = 0;
    int display_offset_y = 0;
    uint32_t refresh_rate = 50;
    uint32_t frame_accumulator_units = 0;
    uint32_t frame_dt_ms = 20;
    uint32_t now_ms = 0;
    uint32_t elapsed_reset_ms = 0;
    uint8_t held_buttons = 0;
    uint8_t pressed_buttons = 0;
    uint8_t previous_held_buttons = 0;
    uint32_t next_timer_id = 1;

    static constexpr size_t kMaximumCachedSounds = 32;
    static constexpr size_t kMaximumSoundCacheBytes = 2U * 1024U * 1024U;
    std::array<CachedSound, kMaximumCachedSounds> sound_cache{};
    size_t sound_cache_bytes = 0;

    Image screen{};
    bool screen_in_internal_ram = false;
    Image* target = nullptr;
    Image* stencil = nullptr;
    PdFont* current_font = nullptr;
    int current_font_ref = LUA_NOREF;
    ClipRect clip{};
    struct Context {
        Image* target = nullptr;
        ClipRect clip{};
        Image* stencil = nullptr;
    };
    std::array<Context, 8> context_stack{};
    size_t context_depth = 0;

    size_t allocated_bytes = 0;
    size_t peak_allocated_bytes = 0;
    Stats runtime_stats{};
    std::array<bool, PdzArchive::MAX_ENTRIES> loaded_modules{};
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
        if (std::strncmp(requested, "CoreLibs/", 9) == 0) return true;
        if (package_mode) {
            const PdzEntry* entry = pdz.findLua(requested);
            if (!entry) {
                setError("import", requested);
                return false;
            }
            const size_t slot = entry->slot;
            if (slot >= loaded_modules.size()) {
                setError("import", "PDZ module index overflow");
                return false;
            }
            if (loaded_modules[slot]) return true;
            uint8_t* bytecode = nullptr;
            size_t bytecode_size = 0;
            char archive_error[128]{};
            const esp_err_t err = pdz.load(*entry, bytecode, bytecode_size,
                                           archive_error, sizeof(archive_error));
            if (err != ESP_OK) {
                setError(entry->name, archive_error);
                return false;
            }
            if (!normalizePlaydateLuaBytecode(bytecode, bytecode_size,
                                              archive_error,
                                              sizeof(archive_error))) {
                setError(entry->name, archive_error);
                heap_caps_free(bytecode);
                return false;
            }
            loaded_modules[slot] = true;
            const int64_t started = esp_timer_get_time();
            ESP_LOGI(TAG, "PDZ exec begin: %s (%u bytes)", entry->name,
                     static_cast<unsigned>(bytecode_size));
            const bool ok = loadBytecode(entry->name, bytecode, bytecode_size);
            const uint32_t elapsed_ms = static_cast<uint32_t>(
                std::max<int64_t>(0, esp_timer_get_time() - started) / 1000);
            ESP_LOGI(TAG, "PDZ exec %s: %s in %lu ms", ok ? "ready" : "failed",
                     entry->name, static_cast<unsigned long>(elapsed_ms));
            heap_caps_free(bytecode);
            if (!ok) loaded_modules[slot] = false;
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

    Image* pushImage() {
        auto* image = static_cast<Image*>(lua_newuserdatauv(lua, sizeof(Image), 0));
        new (image) Image{};
        luaL_getmetatable(lua, kImageMetatable);
        lua_setmetatable(lua, -2);
        return image;
    }

    Image* pushDynamicImage(int width, int height, uint8_t color) {
        if (width <= 0 || height <= 0 || width > 1024 || height > 1024 ||
            static_cast<size_t>(width) * height > 1024U * 1024U) {
            lua_pushnil(lua);
            return nullptr;
        }
        const size_t bytes = static_cast<size_t>(width) * height;
        auto* image = static_cast<Image*>(lua_newuserdatauv(
            lua, sizeof(Image) + bytes, 0));
        new (image) Image{};
        image->width = width;
        image->height = height;
        image->stride = width;
        image->pixels = reinterpret_cast<uint8_t*>(image + 1);
        image->owns_pixels = false;
        std::memset(image->pixels, color, bytes);
        luaL_getmetatable(lua, kImageMetatable);
        lua_setmetatable(lua, -2);
        return image;
    }

    static int cImageGc(lua_State* state) {
        auto* image = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        if (image) releaseImage(*image);
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
    }

    uint8_t mappedColor(uint8_t source, int destination_x, int destination_y) const {
        if (source == Clear) return Clear;
        if (source == Pattern) {
            const unsigned row = static_cast<unsigned>(
                destination_y - pattern_offset_y) & 7U;
            const unsigned column = static_cast<unsigned>(
                destination_x - pattern_offset_x) & 7U;
            source = (draw_pattern[row] & (0x80U >> column)) != 0U
                ? Black : White;
        }
        if (source == Xor) {
            return targetPixel(destination_x, destination_y) == Black
                ? White : Black;
        }
        if (draw_mode == FillWhite) return White;
        if (draw_mode == FillBlack) return Black;
        if (draw_mode == Inverted) return source == Black ? White : Black;
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
        // Celeste composites several persistent 128x128 layer images every
        // frame. Copy their byte pixels row-wise into the logical framebuffer
        // instead of routing every pixel through the generic scaled path.
        if (scale == 1 && fade >= 0.999f && image.pixels && !image.inverted &&
            !stencil && draw_mode != Nxor && target && target->pixels) {
            const int start_x = std::max(0, std::max(clip.x - x, -x));
            const int start_y = std::max(0, std::max(clip.y - y, -y));
            const int end_x = std::min(image.width,
                std::min(clip.x + clip.w - x, target->width - x));
            const int end_y = std::min(image.height,
                std::min(clip.y + clip.h - y, target->height - y));
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
        for (int sy = 0; sy < image.height; ++sy) {
            for (int sx = 0; sx < image.width; ++sx) {
                if (bayer[((y + sy) & 3) * 4 + ((x + sx) & 3)] >= threshold) continue;
                const int source_x = (flip & FlippedX) ? image.width - 1 - sx : sx;
                const int source_y = (flip & FlippedY) ? image.height - 1 - sy : sy;
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
        fillTarget(background_color);
    }

    void flushScreen() {
        if (!canvas || !screen.pixels) return;
        if (display_offset_x != 0 || display_offset_y != 0) {
            canvas->clear(background_color == Black ? gfx::BLACK : gfx::WHITE);
        }
        canvas->reset_clip();
        canvas->draw_indexed2_fast(
            display_offset_x, display_offset_y, screen.width, screen.height,
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
        if (!package_mode || !requested || !requested[0] ||
            std::strstr(requested, "..") || requested[0] == '/') return false;
        const size_t length = std::strlen(requested);
        const size_t extension_length = std::strlen(extension);
        const bool has_extension = length >= extension_length &&
            std::strcmp(requested + length - extension_length, extension) == 0;
        return pdxJoinPath(output, capacity, package_info.path, requested,
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
        for (uint16_t y = 0; y < stored_height; ++y) {
            for (uint16_t x = 0; x < stored_width; ++x) {
                const uint8_t bit = static_cast<uint8_t>(0x80U >> (x & 7U));
                const size_t index = static_cast<size_t>(y) * row_bytes + (x >> 3U);
                if (mask && !(mask[index] & bit)) continue;
                image.pixels[static_cast<size_t>(top + y) * image.stride + left + x] =
                    (bitmap[index] & bit) ? White : Black;
            }
        }
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
                return 1;
            }
            lua_pushnil(state);
            return 1;
        }
        const int width = static_cast<int>(luaL_checkinteger(state, 1));
        const int height = static_cast<int>(luaL_checkinteger(state, 2));
        const uint8_t color = checkPlaydateColor(state, 3, Clear);
        runtime->pushDynamicImage(width, height, color);
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
        const int scale = std::max(1, static_cast<int>(std::lround(luaL_checknumber(state, 4))));
        if (image) runtime->drawImage(*image, x, y, Unflipped, scale);
        return 0;
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

    static int cImageGetSize(lua_State* state) {
        auto* image = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        lua_pushinteger(state, image ? image->width : 0);
        lua_pushinteger(state, image ? image->height : 0);
        return 2;
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
        for (int y = 0; y < source->height; ++y) for (int x = 0; x < source->width; ++x)
            copy->pixels[y * copy->stride + x] = runtime->imagePixel(*source, x, y);
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
            copy->pixels[y * copy->stride + x] = value == Black ? White :
                (value == White ? Black : Clear);
        }
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
                copy->pixels[y * copy->stride + x] = runtime->imagePixel(*source, x, y);
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

    static int cImageTableGetImage(lua_State* state) {
        Impl* runtime = self(state);
        auto* table = static_cast<ImageTable*>(luaL_checkudata(state, 1, kImageTableMetatable));
        const int index = static_cast<int>(luaL_checkinteger(state, 2));
        const int count = table ? (table->asset ? table->asset->frame_count : table->frame_count) : 0;
        if (!table || index < 1 || index > count) {
            lua_pushnil(state);
            return 1;
        }
        lua_getiuservalue(state, 1, 1);
        lua_rawgeti(state, -1, index);
        if (luaL_testudata(state, -1, kImageMetatable)) {
            lua_remove(state, -2);
            return 1;
        }
        lua_pop(state, 1);
        Image* image = nullptr;
        if (table->asset) {
            image = runtime->pushImage();
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
                lua_pushnil(state);
                return 1;
            }
            image = runtime->pushDynamicImage(table->frame_width,
                                              table->frame_height, Clear);
            if (!image || !runtime->decodeSerializedImage(
                    *image, table->data + table_bytes + previous, end - previous,
                    table->frame_width, table->frame_height)) {
                if (image) lua_pop(state, 1);
                lua_pop(state, 1);
                lua_pushnil(state);
                return 1;
            }
        }
        lua_pushvalue(state, -1);
        lua_rawseti(state, -3, index);
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

    static int cGraphicsClear(lua_State* state) {
        Impl* runtime = self(state);
        const uint8_t color = checkPlaydateColor(
            state, 1, runtime->background_color);
        runtime->fillTarget(color);
        return 0;
    }

    static int cSetColor(lua_State* state) {
        self(state)->draw_color = checkPlaydateColor(state, 1, Black);
        return 0;
    }

    static int cSetPattern(lua_State* state) {
        Impl* runtime = self(state);
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
        runtime->pattern_offset_x = static_cast<int>(
            luaL_optinteger(state, 2, 0));
        runtime->pattern_offset_y = static_cast<int>(
            luaL_optinteger(state, 3, 0));
        runtime->draw_color = Pattern;
        return 0;
    }

    static int cSetDrawMode(lua_State* state) {
        self(state)->draw_mode = static_cast<int>(luaL_checkinteger(state, 1));
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
        const size_t glyph_size = glyph_end - glyph_start;
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
                if (!compiledGlyph(*font, codepoint, next_codepoint, glyph) &&
                    codepoint != '?') {
                    compiledGlyph(*font, '?', next_codepoint, glyph);
                }
                line_width += glyph.advance > 0 ? glyph.advance
                    : std::max<int>(1, font->glyph_width);
            } else if (font && font->pico) {
                line_width += 4;
            } else {
                line_width += 6 * (font ? font->scale : 1);
            }
        }
        return std::max(maximum_width, line_width);
    }

    int textWidth(const char* text, size_t length) const {
        return textWidthForFont(current_font, text, length);
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
                if (!compiledGlyph(*current_font, codepoint, next_codepoint,
                                   glyph) && codepoint != '?') {
                    compiledGlyph(*current_font, '?', next_codepoint, glyph);
                }
                drawCompiledGlyph(*current_font, glyph, cursor_x, cursor_y);
                cursor_x += glyph.advance > 0 ? glyph.advance
                    : std::max<int>(1, current_font->glyph_width);
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

    static int cSetClipRect(lua_State* state) {
        Impl* runtime = self(state);
        int x, y, w, h; runtime->readRect(state, 1, x, y, w, h);
        runtime->clip = {x, y, w, h};
        return 0;
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

    static int cSetInverted(lua_State* state) {
        self(state)->inverted_display = lua_toboolean(state, 1) != 0;
        return 0;
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

    static int cGetSecondsSinceEpoch(lua_State* state) {
        lua_pushinteger(state, static_cast<lua_Integer>(std::time(nullptr)));
        return 1;
    }

    static int cGetTime(lua_State* state) {
        const std::time_t now = std::time(nullptr);
        const std::tm* value = std::localtime(&now);
        lua_newtable(state);
        auto set = [&](const char* name, int number) {
            lua_pushinteger(state, number); lua_setfield(state, -2, name);
        };
        if (value) {
            set("year", value->tm_year + 1900); set("month", value->tm_mon + 1);
            set("day", value->tm_mday); set("hour", value->tm_hour);
            set("minute", value->tm_min); set("second", value->tm_sec);
        }
        return 1;
    }

    static int cEpochFromTime(lua_State* state) {
        (void)luaL_checktype(state, 1, LUA_TTABLE);
        lua_pushinteger(state, static_cast<lua_Integer>(std::time(nullptr)));
        return 1;
    }

    static int cGetReduceFlashing(lua_State* state) { lua_pushboolean(state, 1); return 1; }
    static int cGetCrankTicks(lua_State* state) { lua_pushinteger(state, 0); return 1; }
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
            payload_size > 16U * 1024U * 1024U || format > 4U) {
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

        uint8_t block_bytes[2]{};
        if (payload_size < 6U || std::fread(block_bytes, 1, 2, file) != 2) {
            std::fclose(file);
            return false;
        }
        const uint16_t block_align = readLe16(block_bytes);
        if (block_align < 5U || block_align > 4096U) {
            std::fclose(file);
            return false;
        }
        const uint32_t block_count = static_cast<uint32_t>((payload_size - 2U) / block_align);
        const uint32_t samples_per_block = 1U + (block_align - 4U) * 2U;
        const uint64_t total_frames = static_cast<uint64_t>(block_count) * samples_per_block;
        if (block_count == 0 || total_frames > 2U * 1024U * 1024U) {
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
        for (uint32_t block_index = 0; block_index < block_count; ++block_index) {
            if (std::fread(block, 1, block_align, file) != block_align) {
                ok = false; break;
            }
            int predictor = static_cast<int16_t>(readLe16(block));
            int step_index = std::clamp<int>(block[2], 0, 88);
            samples[output++] = static_cast<int16_t>(predictor);
            for (uint16_t byte_index = 4; byte_index < block_align; ++byte_index) {
                for (int shift : {0, 4}) {
                    const int nibble = (block[byte_index] >> shift) & 0x0F;
                    const int step = step_table[step_index];
                    const int delta = (((nibble & 7) * 2 + 1) * step) >> 3;
                    predictor += (nibble & 8) ? -delta : delta;
                    predictor = std::clamp(predictor, -32768, 32767);
                    step_index = std::clamp(step_index + index_table[nibble], 0, 88);
                    samples[output++] = static_cast<int16_t>(predictor);
                }
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
        if (bytes > kMaximumSoundCacheBytes - sound_cache_bytes) {
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

    int createTimer(lua_State* state, int duration_index, int callback_index) {
        const lua_Number duration = std::max<lua_Number>(1, luaL_checknumber(state, duration_index));
        luaL_checktype(state, callback_index, LUA_TFUNCTION);
        lua_newtable(state);
        const int timer = lua_gettop(state);
        lua_pushnumber(state, duration); lua_setfield(state, timer, "duration");
        lua_pushnumber(state, 0); lua_setfield(state, timer, "elapsed");
        lua_pushnumber(state, 0); lua_setfield(state, timer, "currentTime");
        lua_pushboolean(state, 0); lua_setfield(state, timer, "paused");
        lua_pushboolean(state, 0); lua_setfield(state, timer, "removed");
        lua_pushboolean(state, 0); lua_setfield(state, timer, "repeats");
        lua_pushvalue(state, callback_index); lua_setfield(state, timer, "callback");
        setFunction(timer, "pause", cTimerPause); setFunction(timer, "start", cTimerStart);
        setFunction(timer, "reset", cTimerReset); setFunction(timer, "remove", cTimerRemove);
        pushTimerRegistry();
        lua_pushinteger(state, next_timer_id++); lua_pushvalue(state, timer); lua_settable(state, -3);
        lua_pop(state, 1);
        return 1;
    }

    static int cTimerNew(lua_State* state) { return self(state)->createTimer(state, 1, 2); }
    static int cTimerAfter(lua_State* state) { return self(state)->createTimer(state, 1, 2); }

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
            lua_getfield(lua, timer, "updateCallback");
            if (lua_isfunction(lua, -1)) {
                if (lua_pcall(lua, 0, 0, 0) != LUA_OK) { takeLuaError("timer update"); lua_settop(lua,top); return false; }
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
                    if (lua_pcall(lua, 0, 0, 0) != LUA_OK) { takeLuaError("timer callback"); lua_settop(lua,top); return false; }
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
            lua_pushcfunction(lua,cImageGc);lua_setfield(lua,-2,"__gc");
            setFunction(-1,"draw",cImageDraw);setFunction(-1,"drawCentered",cImageDrawCentered);
            setFunction(-1,"drawScaled",cImageDrawScaled);
            setFunction(-1,"drawRotated",cImageDrawRotated);
            setFunction(-1,"drawFaded",cImageDrawFaded);setFunction(-1,"getSize",cImageGetSize);
            setFunction(-1,"clear",cImageClear);setFunction(-1,"copy",cImageCopy);
            setFunction(-1,"setInverted",cImageSetInverted);
            setFunction(-1,"invertedImage",cImageInverted);setFunction(-1,"fadedImage",cImageFaded);
        } lua_pop(lua,1);
        if(luaL_newmetatable(lua,kImageTableMetatable)){
            lua_pushvalue(lua,-1);lua_setfield(lua,-2,"__index");
            setFunction(-1,"getImage",cImageTableGetImage);
            pushFunction(cImageTableLen);lua_setfield(lua,-2,"__len");
        } lua_pop(lua,1);
        if(luaL_newmetatable(lua,kSoundMetatable)){
            lua_pushvalue(lua,-1);lua_setfield(lua,-2,"__index");
            setFunction(-1,"play",cSoundPlay);setFunction(-1,"stop",cSoundStop);
            setFunction(-1,"pause",cSoundPause);setFunction(-1,"isPlaying",cSoundIsPlaying);
            setFunction(-1,"setVolume",cSoundSetVolume);setFunction(-1,"setSample",cSoundSetSample);
            setFunction(-1,"setRate",cSoundSetRate);setFunction(-1,"setLoopRange",cNoop);
            setFunction(-1,"load",cSoundLoad);setFunction(-1,"setStopOnUnderrun",cNoop);
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
        setFunction(playdate,"getTime",cGetTime);setFunction(playdate,"epochFromTime",cEpochFromTime);
        setFunction(playdate,"getReduceFlashing",cGetReduceFlashing);setFunction(playdate,"getCrankTicks",cGetCrankTicks);
        setFunction(playdate,"setCrankSoundsDisabled",cNoop);
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
        setInteger(graphics,"kDrawModeCopy",Copy);setInteger(graphics,"kDrawModeFillWhite",FillWhite);
        setInteger(graphics,"kDrawModeFillBlack",FillBlack);setInteger(graphics,"kDrawModeInverted",Inverted);
        setInteger(graphics,"kDrawModeNXOR",Nxor);setInteger(graphics,"kImageUnflipped",Unflipped);
        setInteger(graphics,"kImageFlippedX",FlippedX);setInteger(graphics,"kImageFlippedY",FlippedY);
        setInteger(graphics,"kImageFlippedXY",FlippedXY);setInteger(graphics,"kStrokeInside",0);setInteger(graphics,"kStrokeOutside",1);
        setFunction(graphics,"_beginFrame",cGraphicsBeginFrame);setFunction(graphics,"_getImageDrawMode",cGetDrawMode);
        setFunction(graphics,"getImageDrawMode",cGetDrawMode);
        setFunction(graphics,"clear",cGraphicsClear);setFunction(graphics,"setColor",cSetColor);
        setFunction(graphics,"setPattern",cSetPattern);
        setFunction(graphics,"setImageDrawMode",cSetDrawMode);setFunction(graphics,"setLineWidth",cSetLineWidth);
        setFunction(graphics,"setStrokeLocation",cSetStrokeLocation);setFunction(graphics,"fillRect",cFillRect);
        setFunction(graphics,"drawRect",cDrawRect);setFunction(graphics,"drawLine",cDrawLine);
        setFunction(graphics,"fillCircleAtPoint",cFillCircle);setFunction(graphics,"drawCircleAtPoint",cDrawCircle);
        setFunction(graphics,"fillCircleInRect",cFillCircleInRect);setFunction(graphics,"drawCircleInRect",cDrawCircleInRect);
        setFunction(graphics,"drawText",cDrawText);setFunction(graphics,"drawTextInRect",cDrawTextInRect);
        setFunction(graphics,"drawTextAligned",cDrawTextAligned);
        setFunction(graphics,"setFont",cSetFont);setFunction(graphics,"pushContext",cPushContext);
        setFunction(graphics,"popContext",cPopContext);setFunction(graphics,"setClipRect",cSetClipRect);
        setFunction(graphics,"clearClipRect",cClearClipRect);setFunction(graphics,"setStencilImage",cSetStencil);
        setFunction(graphics,"clearStencil",cClearStencil);setFunction(graphics,"setBackgroundColor",cSetBackgroundColor);
        lua_newtable(lua);setFunction(-1,"new",cFontNew);lua_setfield(lua,graphics,"font");
        lua_newtable(lua);setFunction(-1,"new",cImageNew);
        setInteger(-1,"kDitherTypeBayer2x2",0);setInteger(-1,"kDitherTypeHorizontalLine",1);setInteger(-1,"kDitherTypeDiagonalLine",2);
        lua_setfield(lua,graphics,"image");
        lua_newtable(lua);setFunction(-1,"new",cImageTableNew);lua_setfield(lua,graphics,"imagetable");
        lua_setfield(lua,playdate,"graphics");

        lua_newtable(lua);
        lua_newtable(lua);setFunction(-1,"new",cSoundNew);lua_setfield(lua,-2,"sample");
        lua_newtable(lua);setFunction(-1,"new",cSoundNew);lua_setfield(lua,-2,"sampleplayer");
        lua_newtable(lua);setFunction(-1,"new",cFilePlayerNew);lua_setfield(lua,-2,"fileplayer");
        lua_setfield(lua,playdate,"sound");
        lua_newtable(lua);setFunction(-1,"new",cTimerNew);setFunction(-1,"performAfterDelay",cTimerAfter);
        setFunction(-1,"updateTimers",cUpdateTimers);lua_setfield(lua,playdate,"timer");
        lua_newtable(lua);setFunction(-1,"read",cDatastoreRead);setFunction(-1,"write",cDatastoreWrite);
        setFunction(-1,"delete",cDatastoreDelete);lua_setfield(lua,playdate,"datastore");
        lua_newtable(lua);const int file=lua_gettop(lua);
        setInteger(file,"kFileRead",0);setInteger(file,"kFileWrite",1);
        setInteger(file,"kFileAppend",2);setInteger(file,"kSeekSet",0);
        setInteger(file,"kSeekFromCurrent",1);setInteger(file,"kSeekFromEnd",2);
        setFunction(file,"open",cFileOpen);setFunction(file,"listFiles",cFileList);
        setFunction(file,"exists",cFileExists);setFunction(file,"isdir",cFileIsDir);
        setFunction(file,"mkdir",cFileMkdir);setFunction(file,"delete",cFileDelete);
        setFunction(file,"getSize",cFileGetSize);setFunction(file,"getType",cFileGetType);
        setFunction(file,"modtime",cFileModtime);setFunction(file,"rename",cFileRename);
        lua_newtable(lua);lua_setfield(lua,file,"file");
        lua_setfield(lua,playdate,"file");
        lua_setglobal(lua,"playdate");
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

    esp_err_t start(gfx::Canvas& target_canvas,audio::Audio& target_audio,
                    storage::Storage& target_storage,Game selected_game,
                    const char* package_path = nullptr) {
        canvas=&target_canvas;audio=&target_audio;storage=&target_storage;game=selected_game;
        last_error[0]='\0';loaded_modules.fill(false);runtime_stats={};
        allocated_bytes=0;peak_allocated_bytes=0;
        package_mode=false;package_info={};pdz.close();
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
        next_timer_id=1;display_scale=1;display_offset_x=display_offset_y=0;
        inverted_display=false;background_color=White;current_font=nullptr;
        current_font_ref=LUA_NOREF;
        if(!resizeScreen(1)){clearSoundCache();setError("startup","screen buffer allocation failed");return ESP_ERR_NO_MEM;}
        resetTargetToScreen();
        lua=lua_newstate(allocator,this);if(!lua){releaseImage(screen);clearSoundCache();setError("startup","could not allocate Lua state");return ESP_ERR_NO_MEM;}
        luaL_openlibs(lua);registerApi();
        size_t compat_size=0;const char* compat=compatSource(compat_size);
        if(!loadBuffer("PogoDate CoreLibs compatibility",compat,compat_size) || !importModule("main")){
            lua_close(lua);lua=nullptr;releaseImage(screen);clearSoundCache();return ESP_FAIL;
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
        clearSoundCache();
        pdz.close();package_mode=false;package_info={};
        releaseImage(screen);
        target=nullptr;stencil=nullptr;current_font=nullptr;
        current_font_ref=LUA_NOREF;canvas=nullptr;audio=nullptr;storage=nullptr;
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
            if(!dispatchInput(pressed_buttons,released)||!callGlobal("playdate","update")){
                is_running=false;
                return 0;
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
uint32_t Runtime::update(uint32_t dt_ms){return impl_?impl_->update(dt_ms):0;}
bool Runtime::running()const{return impl_&&impl_->is_running;}
const char* Runtime::error()const{return impl_&&impl_->last_error[0]?impl_->last_error:"";}
Stats Runtime::stats()const{return impl_?impl_->runtime_stats:Stats{};}

} // namespace pogopo::playdate
