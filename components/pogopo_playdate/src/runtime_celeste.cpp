#include "pogopo/playdate/runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace pogopo::playdate {
namespace {

constexpr char TAG[] = "pogodate";
constexpr uint32_t kMaximumFps = 50;
constexpr size_t kErrorCapacity = 384;
constexpr char kImageMetatable[] = "PogoDate.Image";
constexpr char kImageTableMetatable[] = "PogoDate.ImageTable";
constexpr char kFontMetatable[] = "PogoDate.Font";
constexpr char kSoundMetatable[] = "PogoDate.Sound";
char kTimerRegistryKey;

enum Pixel : uint8_t {
    Clear = 0,
    White = 1,
    Black = 2,
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
};

struct PdFont {
    bool pico = false;
    int scale = 1;
};

struct Sound {
    audio::Effect effect = audio::Effect::Click;
    bool playing = false;
    bool music = false;
    float volume = 1.0f;
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

    bool is_running = false;
    bool inverted_display = false;
    uint8_t draw_color = Black;
    uint8_t background_color = White;
    int draw_mode = Copy;
    int line_width = 1;
    int stroke_location = 0;
    int display_scale = 1;
    int display_offset_x = 0;
    int display_offset_y = 0;
    uint32_t refresh_rate = 50;
    uint32_t frame_accumulator_ms = 0;
    uint32_t frame_dt_ms = 20;
    uint32_t now_ms = 0;
    uint8_t held_buttons = 0;
    uint8_t pressed_buttons = 0;
    uint8_t previous_held_buttons = 0;
    uint32_t next_timer_id = 1;

    Image screen{};
    Image* target = nullptr;
    Image* stencil = nullptr;
    PdFont* current_font = nullptr;
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
    std::array<bool, 64> loaded_modules{};
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

    const EmbeddedSource* sources(size_t& count) const {
        return game == Game::Celeste ? celesteSources(count) : pdsnakeSources(count);
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
                       bool prefer_internal = false) {
        if (width <= 0 || height <= 0 || width > 512 || height > 512) return false;
        const size_t bytes = static_cast<size_t>(width) * height;
        uint8_t* pixels = nullptr;
        if (prefer_internal) {
            pixels = static_cast<uint8_t*>(heap_caps_realloc(
                nullptr, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
        if (!allocateImage(replacement, width, height, background_color,
                           scale == 2)) {
            return false;
        }
        releaseImage(screen);
        screen = replacement;
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
        if (width <= 0 || height <= 0 || width > 512 || height > 512) {
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

    void fillTarget(uint8_t color) {
        if (!target || !target->pixels) return;
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

    static int cImageNew(lua_State* state) {
        Impl* runtime = self(state);
        if (lua_type(state, 1) == LUA_TSTRING) {
            const char* path = lua_tostring(state, 1);
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
        const uint8_t color = static_cast<uint8_t>(luaL_optinteger(state, 3, Clear));
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

    static int cImageDrawScaled(lua_State* state) {
        Impl* runtime = self(state);
        auto* image = static_cast<Image*>(luaL_checkudata(state, 1, kImageMetatable));
        const int x = static_cast<int>(luaL_checknumber(state, 2));
        const int y = static_cast<int>(luaL_checknumber(state, 3));
        const int scale = std::max(1, static_cast<int>(std::lround(luaL_checknumber(state, 4))));
        if (image) runtime->drawImage(*image, x, y, Unflipped, scale);
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
        const uint8_t color = static_cast<uint8_t>(luaL_optinteger(state, 2, Clear));
        if (image && image->pixels) std::memset(image->pixels, color,
            static_cast<size_t>(image->stride) * image->height);
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
        const char* path = luaL_checkstring(state, 1);
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
        if (!table || !table->asset || index < 1 || index > table->asset->frame_count) {
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
        Image* image = runtime->pushImage();
        image->width = table->asset->frame_width;
        image->height = table->asset->frame_height;
        image->asset = table->asset;
        image->frame = index - 1;
        lua_pushvalue(state, -1);
        lua_rawseti(state, -3, index);
        lua_remove(state, -2);
        return 1;
    }

    static int cImageTableLen(lua_State* state) {
        auto* table = static_cast<ImageTable*>(luaL_checkudata(state, 1, kImageTableMetatable));
        lua_pushinteger(state, table && table->asset ? table->asset->frame_count : 0);
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
        const uint8_t color = static_cast<uint8_t>(luaL_optinteger(state, 1, White));
        runtime->fillTarget(color);
        return 0;
    }

    static int cSetColor(lua_State* state) {
        self(state)->draw_color = static_cast<uint8_t>(luaL_checkinteger(state, 1));
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

    static int cFillRect(lua_State* state) {
        Impl* runtime = self(state);
        int x, y, w, h; runtime->readRect(state, 1, x, y, w, h);
        for (int row = 0; row < h; ++row) for (int column = 0; column < w; ++column)
            runtime->putLogicalPixel(x + column, y + row, runtime->draw_color);
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

    int textWidth(const char*, size_t length) const {
        if (!current_font) return static_cast<int>(length) * 6;
        return static_cast<int>(length) * (current_font->pico ? 4 : 6 * current_font->scale);
    }

    void drawText(const char* text, size_t length, int x, int y) {
        if (!text) return;
        int cursor_x = x, cursor_y = y;
        const bool pico = current_font && current_font->pico;
        const int scale = current_font ? current_font->scale : 1;
        const CelesteAsset* font_asset = pico ? findCelesteAsset("Assets/pico") : nullptr;
        for (size_t i = 0; i < length; ++i) {
            const unsigned char character = static_cast<unsigned char>(text[i]);
            if (character == '\n') {
                cursor_x = x;
                cursor_y += pico ? 6 : 8 * scale;
                continue;
            }
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

    static int cFontNew(lua_State* state) {
        const char* path = luaL_optstring(state, 1, "");
        auto* font = static_cast<PdFont*>(lua_newuserdatauv(state, sizeof(PdFont), 0));
        font->pico = path && std::strstr(path, "Assets/pico");
        font->scale = (!font->pico && path && std::strstr(path, "-20-")) ? 2 : 1;
        luaL_getmetatable(state, kFontMetatable);
        lua_setmetatable(state, -2);
        return 1;
    }

    static int cFontGetHeight(lua_State* state) {
        auto* font = static_cast<PdFont*>(luaL_checkudata(state, 1, kFontMetatable));
        lua_pushinteger(state, font && font->pico ? 5 : 7 * (font ? font->scale : 1));
        return 1;
    }

    static int cSetFont(lua_State* state) {
        self(state)->current_font = static_cast<PdFont*>(
            luaL_checkudata(state, 1, kFontMetatable));
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
        self(state)->background_color = static_cast<uint8_t>(luaL_checkinteger(state, 1));
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
        return 0;
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

    static int cButtonJustPressed(lua_State* state) {
        Impl* runtime = self(state);
        const uint8_t button = static_cast<uint8_t>(luaL_checkinteger(state, 1));
        lua_pushboolean(state, (runtime->pressed_buttons & button) != 0);
        return 1;
    }

    static int cButtonIsPressed(lua_State* state) {
        Impl* runtime = self(state);
        const uint8_t button = static_cast<uint8_t>(luaL_checkinteger(state, 1));
        lua_pushboolean(state, (runtime->held_buttons & button) != 0);
        return 1;
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

    static int cSoundNew(lua_State* state) {
        const char* path = lua_type(state, 1) == LUA_TSTRING ? lua_tostring(state, 1) : "";
        auto* original = static_cast<Sound*>(luaL_testudata(state, 1, kSoundMetatable));
        auto* sound = static_cast<Sound*>(lua_newuserdatauv(state, sizeof(Sound), 0));
        new (sound) Sound{};
        if (original) *sound = *original;
        else {
            sound->effect = effectForPath(path);
            sound->music = path && std::strstr(path, "Sounds/music");
        }
        luaL_getmetatable(state, kSoundMetatable);
        lua_setmetatable(state, -2);
        return 1;
    }

    static int cSoundPlay(lua_State* state) {
        Impl* runtime = self(state);
        auto* sound = static_cast<Sound*>(luaL_checkudata(state, 1, kSoundMetatable));
        if (sound) {
            sound->playing = true;
            if (!sound->music && runtime->audio) runtime->audio->play(sound->effect);
        }
        lua_pushboolean(state, sound != nullptr);
        return 1;
    }

    static int cSoundStop(lua_State* state) {
        auto* sound = static_cast<Sound*>(luaL_checkudata(state, 1, kSoundMetatable));
        if (sound) sound->playing = false;
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

    static int cSoundSetSample(lua_State* state) {
        auto* sound = static_cast<Sound*>(luaL_checkudata(state, 1, kSoundMetatable));
        auto* sample = static_cast<Sound*>(luaL_checkudata(state, 2, kSoundMetatable));
        if (sound && sample) { sound->effect = sample->effect; sound->music = sample->music; }
        return 0;
    }

    static int cSoundLoad(lua_State* state) {
        auto* sound = static_cast<Sound*>(luaL_checkudata(state, 1, kSoundMetatable));
        const char* path = luaL_checkstring(state, 2);
        if (sound) { sound->effect = effectForPath(path); sound->music = std::strstr(path, "Sounds/music"); }
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

    void savePath(char* path, size_t capacity, const char* name, const char* extension) const {
        const char* mount = storage ? storage->mountPoint() : "/sdcard";
        const char* prefix = game == Game::Celeste ? "celeste" : "pdsnake";
        char safe[48]{};
        size_t out = 0;
        for (const char* value = name && name[0] ? name : "data"; *value && out + 1 < sizeof(safe); ++value) {
            const char c = *value;
            safe[out++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_') ? c : '_';
        }
        std::snprintf(path, capacity, "%s/pogodate/%s_%s.%s", mount, prefix, safe, extension);
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
        char directory[208]{}; std::snprintf(directory,sizeof(directory),"%s/pogodate",runtime->storage->mountPoint()); mkdir(directory,0775);
        char path[240]{},temporary[240]{}; runtime->savePath(path,sizeof(path),name,"lua"); runtime->savePath(temporary,sizeof(temporary),name,"tmp");
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
            lua_pushvalue(lua,-1);lua_setfield(lua,-2,"__index");setFunction(-1,"getHeight",cFontGetHeight);
        } lua_pop(lua,1);
        if(luaL_newmetatable(lua,kImageMetatable)){
            lua_pushvalue(lua,-1);lua_setfield(lua,-2,"__index");
            lua_pushcfunction(lua,cImageGc);lua_setfield(lua,-2,"__gc");
            setFunction(-1,"draw",cImageDraw);setFunction(-1,"drawScaled",cImageDrawScaled);
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
            setFunction(-1,"load",cSoundLoad);setFunction(-1,"setStopOnUnderrun",cNoop);
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
        setFunction(playdate,"buttonJustPressed",cButtonJustPressed);setFunction(playdate,"buttonIsPressed",cButtonIsPressed);
        setFunction(playdate,"getSystemMenu",cGetSystemMenu);setFunction(playdate,"setMenuImage",cSetMenuImage);
        setFunction(playdate,"drawFPS",cDrawFps);setFunction(playdate,"getSecondsSinceEpoch",cGetSecondsSinceEpoch);
        setFunction(playdate,"getTime",cGetTime);setFunction(playdate,"epochFromTime",cEpochFromTime);
        setFunction(playdate,"getReduceFlashing",cGetReduceFlashing);setFunction(playdate,"getCrankTicks",cGetCrankTicks);
        setFunction(playdate,"setCrankSoundsDisabled",cNoop);
        lua_newtable(lua);
        lua_pushstring(lua,game==Game::Celeste?"Celeste Classic":"PDSnake");lua_setfield(lua,-2,"name");
        lua_pushstring(lua,game==Game::Celeste?"1.0.3":"1.2");lua_setfield(lua,-2,"version");
        lua_pushstring(lua,game==Game::Celeste?"8":"7");lua_setfield(lua,-2,"buildNumber");
        lua_pushstring(lua,game==Game::Celeste?"HTeuMeuLeu":"Brett Chalupa");lua_setfield(lua,-2,"author");
        lua_setfield(lua,playdate,"metadata");

        lua_newtable(lua);setFunction(-1,"getWidth",cDisplayWidth);setFunction(-1,"getHeight",cDisplayHeight);
        setFunction(-1,"setRefreshRate",cSetRefreshRate);setFunction(-1,"setScale",cSetScale);
        setFunction(-1,"setOffset",cSetOffset);setFunction(-1,"setInverted",cSetInverted);
        lua_setfield(lua,playdate,"display");

        lua_newtable(lua);const int graphics=lua_gettop(lua);
        setInteger(graphics,"kColorClear",Clear);setInteger(graphics,"kColorWhite",White);setInteger(graphics,"kColorBlack",Black);
        setInteger(graphics,"kDrawModeCopy",Copy);setInteger(graphics,"kDrawModeFillWhite",FillWhite);
        setInteger(graphics,"kDrawModeFillBlack",FillBlack);setInteger(graphics,"kDrawModeInverted",Inverted);
        setInteger(graphics,"kDrawModeNXOR",Nxor);setInteger(graphics,"kImageUnflipped",Unflipped);
        setInteger(graphics,"kImageFlippedX",FlippedX);setInteger(graphics,"kImageFlippedY",FlippedY);
        setInteger(graphics,"kImageFlippedXY",FlippedXY);setInteger(graphics,"kStrokeInside",0);setInteger(graphics,"kStrokeOutside",1);
        setFunction(graphics,"_beginFrame",cGraphicsBeginFrame);setFunction(graphics,"_getImageDrawMode",cGetDrawMode);
        setFunction(graphics,"clear",cGraphicsClear);setFunction(graphics,"setColor",cSetColor);
        setFunction(graphics,"setImageDrawMode",cSetDrawMode);setFunction(graphics,"setLineWidth",cSetLineWidth);
        setFunction(graphics,"setStrokeLocation",cSetStrokeLocation);setFunction(graphics,"fillRect",cFillRect);
        setFunction(graphics,"drawRect",cDrawRect);setFunction(graphics,"drawLine",cDrawLine);
        setFunction(graphics,"fillCircleAtPoint",cFillCircle);setFunction(graphics,"drawCircleAtPoint",cDrawCircle);
        setFunction(graphics,"fillCircleInRect",cFillCircleInRect);setFunction(graphics,"drawCircleInRect",cDrawCircleInRect);
        setFunction(graphics,"drawText",cDrawText);setFunction(graphics,"drawTextInRect",cDrawTextInRect);
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
        lua_newtable(lua);setFunction(-1,"new",cSoundNew);lua_setfield(lua,-2,"fileplayer");
        lua_setfield(lua,playdate,"sound");
        lua_newtable(lua);setFunction(-1,"new",cTimerNew);setFunction(-1,"performAfterDelay",cTimerAfter);
        setFunction(-1,"updateTimers",cUpdateTimers);lua_setfield(lua,playdate,"timer");
        lua_newtable(lua);setFunction(-1,"read",cDatastoreRead);setFunction(-1,"write",cDatastoreWrite);
        setFunction(-1,"delete",cDatastoreDelete);lua_setfield(lua,playdate,"datastore");
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
                    storage::Storage& target_storage,Game selected_game) {
        canvas=&target_canvas;audio=&target_audio;storage=&target_storage;game=selected_game;
        last_error[0]='\0';loaded_modules.fill(false);runtime_stats={};
        allocated_bytes=0;peak_allocated_bytes=0;
        refresh_rate=game==Game::Celeste?30:50;runtime_stats.requested_fps=refresh_rate;
        frame_accumulator_ms=0;now_ms=0;held_buttons=pressed_buttons=previous_held_buttons=0;
        next_timer_id=1;display_scale=1;display_offset_x=display_offset_y=0;
        inverted_display=false;background_color=White;current_font=nullptr;
        if(!resizeScreen(1)){setError("startup","screen buffer allocation failed");return ESP_ERR_NO_MEM;}
        resetTargetToScreen();
        lua=lua_newstate(allocator,this);if(!lua){releaseImage(screen);setError("startup","could not allocate Lua state");return ESP_ERR_NO_MEM;}
        luaL_openlibs(lua);registerApi();
        size_t compat_size=0;const char* compat=compatSource(compat_size);
        if(!loadBuffer("PogoDate CoreLibs compatibility",compat,compat_size) || !importModule("main")){
            lua_close(lua);lua=nullptr;releaseImage(screen);return ESP_FAIL;
        }
        lua_gc(lua, LUA_GCGEN, 20, 100);
        is_running=true;
        ESP_LOGI(TAG,"PogoDate Lite ready: %s source Lua 5.4, 400x240, logic=%lu FPS LCD cap=30",
                 game==Game::Celeste?"Celeste Classic 1.0.3":"PDSnake 1.2",
                 static_cast<unsigned long>(refresh_rate));
        return ESP_OK;
    }

    void stop() {
        if(lua&&is_running)callGlobal("playdate","gameWillTerminate",true);
        is_running=false;if(lua){lua_close(lua);lua=nullptr;}
        releaseImage(screen);
        target=nullptr;stencil=nullptr;current_font=nullptr;canvas=nullptr;audio=nullptr;storage=nullptr;
    }

    uint32_t update(uint32_t dt_ms) {
        if(!lua||!is_running)return 0;
        frame_accumulator_ms=std::min<uint32_t>(frame_accumulator_ms+dt_ms,250U);now_ms+=dt_ms;
        const uint32_t interval=std::max<uint32_t>(1U,1000U/std::max<uint32_t>(1U,refresh_rate));
        uint32_t produced=0;
        // Never run multiple slow Lua frames before returning to AppManager.
        // The old three-frame catch-up loop made a 5 FPS VM reach the LCD only
        // once per three updates, which is why the visible rate was ~1-2 FPS.
        while(frame_accumulator_ms>=interval&&produced<1U){
            frame_accumulator_ms-=interval;frame_dt_ms=interval;
            const uint8_t released=static_cast<uint8_t>(previous_held_buttons&~held_buttons);
            const int64_t started=esp_timer_get_time();
            if(!dispatchInput(pressed_buttons,released)||!callGlobal("playdate","update")){is_running=false;break;}
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
void Runtime::stop(){if(impl_)impl_->stop();}
void Runtime::setInput(uint8_t held_mask,uint8_t pressed_mask){if(!impl_)return;impl_->held_buttons=held_mask;impl_->pressed_buttons=static_cast<uint8_t>(impl_->pressed_buttons|pressed_mask);}
uint32_t Runtime::update(uint32_t dt_ms){return impl_?impl_->update(dt_ms):0;}
bool Runtime::running()const{return impl_&&impl_->is_running;}
const char* Runtime::error()const{return impl_&&impl_->last_error[0]?impl_->last_error:"";}
Stats Runtime::stats()const{return impl_?impl_->runtime_stats:Stats{};}

} // namespace pogopo::playdate
