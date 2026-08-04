#include "pogopo/playdate/runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <sys/stat.h>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "embedded_source.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace pogopo::playdate {
namespace {

constexpr char TAG[] = "pogodate";
constexpr uint32_t kDefaultFps = 50;
constexpr uint32_t kMaximumFps = 50;
constexpr size_t kErrorCapacity = 256;
constexpr char kImageMetatable[] = "PogoDate.Image";
constexpr char kFontMetatable[] = "PogoDate.Font";
constexpr char kSoundMetatable[] = "PogoDate.Sound";
char kTimerRegistryKey;

enum class ImageKind : uint8_t {
    Blank,
    Snake,
    QrCode,
};

struct Image {
    ImageKind kind = ImageKind::Blank;
    int width = 0;
    int height = 0;
};

struct Font {
    int scale = 1;
};

struct Sound {
    audio::Effect effect = audio::Effect::Click;
    float volume = 1.0f;
};

const EmbeddedSource* findSource(const char* requested, size_t& index) {
    if (!requested || !requested[0]) return nullptr;

    char normalized[96]{};
    const size_t length = std::strlen(requested);
    if (length >= sizeof(normalized) - 5U) return nullptr;
    std::memcpy(normalized, requested, length);
    if (length < 4U || std::strcmp(requested + length - 4U, ".lua") != 0) {
        std::memcpy(normalized + length, ".lua", 5U);
    }

    size_t count = 0;
    const EmbeddedSource* sources = pdsnakeSources(count);
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(sources[i].name, normalized) == 0) {
            index = i;
            return &sources[i];
        }
    }
    return nullptr;
}

std::string adaptPlaydateLua(const char* source, size_t size) {
    std::string output;
    output.reserve(size + 256U);

    size_t offset = 0;
    while (offset < size) {
        const size_t line_end = [&]() {
            const char* newline = static_cast<const char*>(
                std::memchr(source + offset, '\n', size - offset));
            return newline ? static_cast<size_t>(newline - source) : size;
        }();

        std::string line(source + offset, line_end - offset);
        const size_t first = line.find_first_not_of(" \t");
        const size_t plus = line.find(" += ");
        const size_t minus = line.find(" -= ");
        const size_t operation = plus != std::string::npos ? plus : minus;

        if (first != std::string::npos && operation != std::string::npos &&
            operation > first) {
            const char symbol = plus != std::string::npos ? '+' : '-';
            const std::string indent = line.substr(0, first);
            const std::string left = line.substr(first, operation - first);
            const std::string right = line.substr(operation + 4U);
            output += indent;
            output += left;
            output += " = ";
            output += left;
            output += ' ';
            output += symbol;
            output += ' ';
            output += right;
        } else {
            output += line;
        }

        if (line_end < size) output.push_back('\n');
        offset = line_end + (line_end < size ? 1U : 0U);
    }
    return output;
}

} // namespace

struct Runtime::Impl {
    lua_State* lua = nullptr;
    gfx::Canvas* canvas = nullptr;
    audio::Audio* audio = nullptr;
    storage::Storage* storage = nullptr;

    bool is_running = false;
    bool inverted = false;
    gfx::Color draw_color = gfx::BLACK;
    int font_scale = 1;
    int line_width = 1;
    uint32_t refresh_rate = kDefaultFps;
    uint32_t frame_accumulator_ms = 0;
    uint32_t frame_dt_ms = 20;
    uint32_t now_ms = 0;
    uint8_t held_buttons = 0;
    uint8_t pressed_buttons = 0;
    uint32_t next_timer_id = 1;

    size_t allocated_bytes = 0;
    size_t peak_allocated_bytes = 0;
    Stats runtime_stats{};
    std::array<bool, 32> loaded_modules{};
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
                runtime->allocated_bytes =
                    old_size <= runtime->allocated_bytes
                        ? runtime->allocated_bytes - old_size
                        : 0;
            }
            return nullptr;
        }

        void* result = heap_caps_realloc(
            pointer, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!result) {
            result = heap_caps_realloc(pointer, new_size, MALLOC_CAP_8BIT);
        }
        if (!result) return nullptr;

        if (new_size >= old_size) {
            runtime->allocated_bytes += new_size - old_size;
        } else {
            runtime->allocated_bytes -=
                std::min(runtime->allocated_bytes, old_size - new_size);
        }
        runtime->peak_allocated_bytes =
            std::max(runtime->peak_allocated_bytes, runtime->allocated_bytes);
        return result;
    }

    void pushFunction(lua_CFunction function) {
        lua_pushlightuserdata(lua, this);
        lua_pushcclosure(lua, function, 1);
    }

    void setFunction(int table_index, const char* name,
                     lua_CFunction function) {
        table_index = lua_absindex(lua, table_index);
        pushFunction(function);
        lua_setfield(lua, table_index, name);
    }

    gfx::Color effective(gfx::Color color) const {
        if (!inverted) return color;
        return color == gfx::BLACK ? gfx::WHITE : gfx::BLACK;
    }

    void setError(const char* context, const char* message) {
        std::snprintf(last_error, sizeof(last_error), "%s: %s",
                      context ? context : "Lua",
                      message ? message : "unknown error");
        ++runtime_stats.errors;
        ESP_LOGE(TAG, "%s", last_error);
    }

    bool takeLuaError(const char* context) {
        const char* message = lua_tostring(lua, -1);
        setError(context, message);
        lua_pop(lua, 1);
        return false;
    }

    bool callGlobal(const char* table_name, const char* function_name,
                    bool optional = false) {
        const int top = lua_gettop(lua);
        lua_getglobal(lua, table_name);
        if (!lua_istable(lua, -1)) {
            lua_settop(lua, top);
            if (!optional) setError(function_name, "missing table");
            return optional;
        }
        lua_getfield(lua, -1, function_name);
        if (!lua_isfunction(lua, -1)) {
            lua_settop(lua, top);
            if (!optional) setError(function_name, "missing function");
            return optional;
        }
        if (lua_pcall(lua, 0, 0, 0) != LUA_OK) {
            const bool result = takeLuaError(function_name);
            lua_settop(lua, top);
            return result;
        }
        lua_settop(lua, top);
        return true;
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

        const std::string adapted = adaptPlaydateLua(source->data, source->size);
        if (luaL_loadbuffer(lua, adapted.data(), adapted.size(),
                            source->name) != LUA_OK) {
            loaded_modules[index] = false;
            return takeLuaError(source->name);
        }
        if (lua_pcall(lua, 0, 0, 0) != LUA_OK) {
            loaded_modules[index] = false;
            return takeLuaError(source->name);
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

    static int cGetCurrentTime(lua_State* state) {
        lua_pushinteger(state, static_cast<lua_Integer>(self(state)->now_ms));
        return 1;
    }

    static int cButtonJustPressed(lua_State* state) {
        Impl* runtime = self(state);
        const uint8_t button = static_cast<uint8_t>(
            luaL_checkinteger(state, 1));
        lua_pushboolean(state, (runtime->pressed_buttons & button) != 0);
        return 1;
    }

    static int cButtonIsPressed(lua_State* state) {
        Impl* runtime = self(state);
        const uint8_t button = static_cast<uint8_t>(
            luaL_checkinteger(state, 1));
        lua_pushboolean(state, (runtime->held_buttons & button) != 0);
        return 1;
    }

    static int cDisplayWidth(lua_State* state) {
        lua_pushinteger(state, 400);
        return 1;
    }

    static int cDisplayHeight(lua_State* state) {
        lua_pushinteger(state, 240);
        return 1;
    }

    static int cSetRefreshRate(lua_State* state) {
        Impl* runtime = self(state);
        const int requested = static_cast<int>(luaL_checkinteger(state, 1));
        runtime->refresh_rate = static_cast<uint32_t>(
            std::clamp(requested, 1, static_cast<int>(kMaximumFps)));
        runtime->runtime_stats.requested_fps = runtime->refresh_rate;
        return 0;
    }

    static int cSetInverted(lua_State* state) {
        self(state)->inverted = lua_toboolean(state, 1) != 0;
        return 0;
    }

    static int cGraphicsClear(lua_State* state) {
        Impl* runtime = self(state);
        gfx::Color color = gfx::WHITE;
        if (!lua_isnoneornil(state, 1)) {
            color = lua_tointeger(state, 1) == 0 ? gfx::BLACK : gfx::WHITE;
        }
        runtime->canvas->clear(runtime->effective(color));
        return 0;
    }

    static int cSetColor(lua_State* state) {
        Impl* runtime = self(state);
        runtime->draw_color =
            luaL_checkinteger(state, 1) == 0 ? gfx::BLACK : gfx::WHITE;
        return 0;
    }

    static int cSetLineWidth(lua_State* state) {
        self(state)->line_width =
            std::clamp(static_cast<int>(luaL_checkinteger(state, 1)), 1, 4);
        return 0;
    }

    static int cFillRect(lua_State* state) {
        Impl* runtime = self(state);
        const int x = static_cast<int>(luaL_checknumber(state, 1));
        const int y = static_cast<int>(luaL_checknumber(state, 2));
        const int width = static_cast<int>(luaL_checknumber(state, 3));
        const int height = static_cast<int>(luaL_checknumber(state, 4));
        runtime->canvas->fill_rect(
            x, y, width, height, runtime->effective(runtime->draw_color));
        return 0;
    }

    static int cFillCircle(lua_State* state) {
        Impl* runtime = self(state);
        const int x = static_cast<int>(std::lround(luaL_checknumber(state, 1)));
        const int y = static_cast<int>(std::lround(luaL_checknumber(state, 2)));
        const int radius =
            static_cast<int>(std::lround(luaL_checknumber(state, 3)));
        runtime->canvas->fill_circle(
            x, y, radius, runtime->effective(runtime->draw_color));
        return 0;
    }

    static int cDrawCircleInRect(lua_State* state) {
        Impl* runtime = self(state);
        const int x = static_cast<int>(luaL_checknumber(state, 1));
        const int y = static_cast<int>(luaL_checknumber(state, 2));
        const int width = static_cast<int>(luaL_checknumber(state, 3));
        const int height = static_cast<int>(luaL_checknumber(state, 4));
        const int radius = std::max(0, std::min(width, height) / 2 - 1);
        const int cx = x + width / 2;
        const int cy = y + height / 2;
        const gfx::Color color = runtime->effective(runtime->draw_color);
        for (int line = 0; line < runtime->line_width; ++line) {
            runtime->canvas->draw_circle(cx, cy, std::max(0, radius - line),
                                         color);
        }
        return 0;
    }

    void drawText(const char* text, size_t length, int x, int y) {
        if (!text) return;
        size_t line_start = 0;
        int line = 0;
        while (line_start <= length) {
            size_t line_end = line_start;
            while (line_end < length && text[line_end] != '\n') ++line_end;
            char buffer[192]{};
            const size_t count =
                std::min(line_end - line_start, sizeof(buffer) - 1U);
            std::memcpy(buffer, text + line_start, count);
            canvas->draw_text(x, y + line * (font_scale * 9), buffer,
                              gfx::font5x7(), effective(draw_color),
                              font_scale);
            if (line_end >= length) break;
            line_start = line_end + 1U;
            ++line;
        }
    }

    static int cDrawText(lua_State* state) {
        Impl* runtime = self(state);
        size_t length = 0;
        const char* text = luaL_tolstring(state, 1, &length);
        const int x = static_cast<int>(luaL_checknumber(state, 2));
        const int y = static_cast<int>(luaL_checknumber(state, 3));
        runtime->drawText(text, length, x, y);
        lua_pop(state, 1);
        return 0;
    }

    static int cFontNew(lua_State* state) {
        const char* path = luaL_optstring(state, 1, "");
        auto* font = static_cast<Font*>(
            lua_newuserdatauv(state, sizeof(Font), 0));
        font->scale = std::strstr(path, "-20-") ? 2 : 1;
        luaL_getmetatable(state, kFontMetatable);
        lua_setmetatable(state, -2);
        return 1;
    }

    static int cSetFont(lua_State* state) {
        Impl* runtime = self(state);
        auto* font = static_cast<Font*>(
            luaL_checkudata(state, 1, kFontMetatable));
        runtime->font_scale = font ? std::clamp(font->scale, 1, 3) : 1;
        return 0;
    }

    static int cImageNew(lua_State* state) {
        auto* image = static_cast<Image*>(
            lua_newuserdatauv(state, sizeof(Image), 0));
        new (image) Image{};
        if (lua_type(state, 1) == LUA_TSTRING) {
            const char* path = lua_tostring(state, 1);
            if (path && std::strstr(path, "qrcode")) {
                image->kind = ImageKind::QrCode;
                image->width = 88;
                image->height = 88;
            } else if (path && std::strstr(path, "snake")) {
                image->kind = ImageKind::Snake;
                image->width = 26;
                image->height = 26;
            } else {
                image->width = 32;
                image->height = 32;
            }
        } else {
            image->width = static_cast<int>(luaL_optinteger(state, 1, 0));
            image->height = static_cast<int>(luaL_optinteger(state, 2, 0));
        }
        luaL_getmetatable(state, kImageMetatable);
        lua_setmetatable(state, -2);
        return 1;
    }

    void drawSnakeImage(int x, int y) {
        const gfx::Color black = effective(gfx::BLACK);
        const gfx::Color white = effective(gfx::WHITE);
        canvas->fill_circle(x + 7, y + 7, 6, black);
        canvas->fill_rect(x + 8, y + 7, 13, 7, black);
        canvas->fill_rect(x + 15, y + 12, 7, 8, black);
        canvas->fill_rect(x + 8, y + 18, 12, 6, black);
        canvas->draw_pixel(x + 5, y + 5, white);
        canvas->draw_pixel(x + 9, y + 5, white);
    }

    static bool qrFinderCell(int x, int y, int origin_x, int origin_y) {
        const int dx = x - origin_x;
        const int dy = y - origin_y;
        if (dx < 0 || dy < 0 || dx >= 7 || dy >= 7) return false;
        return dx == 0 || dy == 0 || dx == 6 || dy == 6 ||
               (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4);
    }

    void drawQrImage(int x, int y) {
        constexpr int cells = 21;
        constexpr int scale = 4;
        canvas->fill_rect(x, y, cells * scale + 4, cells * scale + 4,
                          effective(gfx::WHITE));
        for (int row = 0; row < cells; ++row) {
            for (int column = 0; column < cells; ++column) {
                const bool finder =
                    qrFinderCell(column, row, 0, 0) ||
                    qrFinderCell(column, row, cells - 7, 0) ||
                    qrFinderCell(column, row, 0, cells - 7);
                const uint32_t mixed =
                    static_cast<uint32_t>(column * 17 + row * 31 +
                                          column * row * 7 + 0x5A);
                const bool data = ((mixed ^ (mixed >> 3U)) & 0x03U) == 0;
                if (finder || data) {
                    canvas->fill_rect(x + 2 + column * scale,
                                      y + 2 + row * scale,
                                      scale, scale, effective(gfx::BLACK));
                }
            }
        }
    }

    static int cImageDraw(lua_State* state) {
        Impl* runtime = self(state);
        auto* image = static_cast<Image*>(
            luaL_checkudata(state, 1, kImageMetatable));
        const int x = static_cast<int>(luaL_checknumber(state, 2));
        const int y = static_cast<int>(luaL_checknumber(state, 3));
        if (!image) return 0;
        if (image->kind == ImageKind::Snake) runtime->drawSnakeImage(x, y);
        if (image->kind == ImageKind::QrCode) runtime->drawQrImage(x, y);
        return 0;
    }

    static int cImageGetSize(lua_State* state) {
        auto* image = static_cast<Image*>(
            luaL_checkudata(state, 1, kImageMetatable));
        lua_pushinteger(state, image ? image->width : 0);
        lua_pushinteger(state, image ? image->height : 0);
        return 2;
    }

    static int cPushContext(lua_State*) { return 0; }
    static int cPopContext(lua_State*) { return 0; }
    static int cSetMenuImage(lua_State*) { return 0; }
    static int cDrawFps(lua_State*) { return 0; }

    static int cSoundNew(lua_State* state) {
        const char* path = luaL_optstring(state, 1, "");
        auto* sound = static_cast<Sound*>(
            lua_newuserdatauv(state, sizeof(Sound), 0));
        new (sound) Sound{};
        if (std::strstr(path, "apple")) sound->effect = audio::Effect::Coin;
        else if (std::strstr(path, "death")) sound->effect = audio::Effect::Error;
        else sound->effect = audio::Effect::Click;
        luaL_getmetatable(state, kSoundMetatable);
        lua_setmetatable(state, -2);
        return 1;
    }

    static int cSoundPlay(lua_State* state) {
        Impl* runtime = self(state);
        auto* sound = static_cast<Sound*>(
            luaL_checkudata(state, 1, kSoundMetatable));
        lua_pushboolean(state, sound && runtime->audio->play(sound->effect));
        return 1;
    }

    static int cSoundSetVolume(lua_State* state) {
        auto* sound = static_cast<Sound*>(
            luaL_checkudata(state, 1, kSoundMetatable));
        if (sound) {
            sound->volume = static_cast<float>(
                std::clamp<double>(
                    static_cast<double>(luaL_checknumber(state, 2)),
                    0.0, 1.0));
        }
        return 0;
    }

    void pushTimerRegistry() {
        lua_rawgetp(lua, LUA_REGISTRYINDEX, &kTimerRegistryKey);
    }

    static int cTimerPause(lua_State* state) {
        luaL_checktype(state, 1, LUA_TTABLE);
        lua_pushboolean(state, 1);
        lua_setfield(state, 1, "paused");
        return 0;
    }

    static int cTimerStart(lua_State* state) {
        luaL_checktype(state, 1, LUA_TTABLE);
        lua_pushboolean(state, 0);
        lua_setfield(state, 1, "paused");
        return 0;
    }

    static int cTimerReset(lua_State* state) {
        luaL_checktype(state, 1, LUA_TTABLE);
        lua_pushnumber(state, 0);
        lua_setfield(state, 1, "elapsed");
        return 0;
    }

    static int cTimerNew(lua_State* state) {
        Impl* runtime = self(state);
        const lua_Number duration = std::max<lua_Number>(
            1, luaL_checknumber(state, 1));
        luaL_checktype(state, 2, LUA_TFUNCTION);

        lua_newtable(state);
        const int timer = lua_gettop(state);
        lua_pushnumber(state, duration);
        lua_setfield(state, timer, "duration");
        lua_pushnumber(state, 0);
        lua_setfield(state, timer, "elapsed");
        lua_pushboolean(state, 0);
        lua_setfield(state, timer, "paused");
        lua_pushboolean(state, 0);
        lua_setfield(state, timer, "repeats");
        lua_pushvalue(state, 2);
        lua_setfield(state, timer, "callback");
        runtime->setFunction(timer, "pause", cTimerPause);
        runtime->setFunction(timer, "start", cTimerStart);
        runtime->setFunction(timer, "reset", cTimerReset);

        runtime->pushTimerRegistry();
        lua_pushinteger(state, runtime->next_timer_id++);
        lua_pushvalue(state, timer);
        lua_settable(state, -3);
        lua_pop(state, 1);
        return 1;
    }

    bool updateTimers() {
        const int top = lua_gettop(lua);
        pushTimerRegistry();
        if (!lua_istable(lua, -1)) {
            lua_settop(lua, top);
            setError("timer", "registry missing");
            return false;
        }

        lua_pushnil(lua);
        while (lua_next(lua, -2) != 0) {
            if (!lua_istable(lua, -1)) {
                lua_pop(lua, 1);
                continue;
            }

            const int timer = lua_gettop(lua);
            lua_getfield(lua, timer, "paused");
            const bool paused = lua_toboolean(lua, -1) != 0;
            lua_pop(lua, 1);
            if (paused) {
                lua_pop(lua, 1);
                continue;
            }

            lua_getfield(lua, timer, "duration");
            const lua_Number duration = std::max<lua_Number>(
                1, lua_tonumber(lua, -1));
            lua_pop(lua, 1);
            lua_getfield(lua, timer, "elapsed");
            lua_Number elapsed = lua_tonumber(lua, -1) + frame_dt_ms;
            lua_pop(lua, 1);

            if (elapsed < duration) {
                lua_pushnumber(lua, elapsed);
                lua_setfield(lua, timer, "elapsed");
                lua_pop(lua, 1);
                continue;
            }

            lua_getfield(lua, timer, "repeats");
            const bool repeats = lua_toboolean(lua, -1) != 0;
            lua_pop(lua, 1);
            elapsed = repeats ? std::fmod(elapsed, duration) : duration;
            lua_pushnumber(lua, elapsed);
            lua_setfield(lua, timer, "elapsed");
            if (!repeats) {
                lua_pushboolean(lua, 1);
                lua_setfield(lua, timer, "paused");
            }

            lua_getfield(lua, timer, "callback");
            if (lua_isfunction(lua, -1)) {
                if (lua_pcall(lua, 0, 0, 0) != LUA_OK) {
                    takeLuaError("timer callback");
                    lua_settop(lua, top);
                    return false;
                }
            } else {
                lua_pop(lua, 1);
            }
            lua_pop(lua, 1);
        }
        lua_settop(lua, top);
        return true;
    }

    static int cUpdateTimers(lua_State* state) {
        Impl* runtime = self(state);
        if (!runtime->updateTimers()) {
            return luaL_error(state, "%s", runtime->last_error);
        }
        return 0;
    }

    void savePath(char* path, size_t capacity, const char* suffix) const {
        const char* mount = storage ? storage->mountPoint() : "/sdcard";
        std::snprintf(path, capacity, "%s/pogodate/%s", mount, suffix);
    }

    static int cDatastoreRead(lua_State* state) {
        Impl* runtime = self(state);
        if (!runtime->storage || !runtime->storage->mounted()) {
            lua_pushnil(state);
            return 1;
        }

        char path[224]{};
        runtime->savePath(path, sizeof(path), "pdsnake.save");
        FILE* file = std::fopen(path, "rb");
        if (!file) {
            lua_pushnil(state);
            return 1;
        }

        lua_newtable(state);
        char line[256]{};
        while (std::fgets(line, sizeof(line), file)) {
            char* equals = std::strchr(line, '=');
            if (!equals || equals == line || !equals[1] || equals[2] != ':') {
                continue;
            }
            *equals = '\0';
            char* value = equals + 3;
            value[std::strcspn(value, "\r\n")] = '\0';
            if (equals[1] == 'b') {
                lua_pushboolean(state, std::strcmp(value, "1") == 0);
            } else if (equals[1] == 'n') {
                lua_pushnumber(state, std::strtod(value, nullptr));
            } else if (equals[1] == 's') {
                lua_pushstring(state, value);
            } else {
                continue;
            }
            lua_setfield(state, -2, line);
        }
        std::fclose(file);
        return 1;
    }

    static int cDatastoreWrite(lua_State* state) {
        Impl* runtime = self(state);
        luaL_checktype(state, 1, LUA_TTABLE);
        if (!runtime->storage || !runtime->storage->mounted()) {
            lua_pushboolean(state, 0);
            return 1;
        }

        char directory[208]{};
        runtime->savePath(directory, sizeof(directory), "");
        const size_t length = std::strlen(directory);
        if (length > 0 && directory[length - 1U] == '/') {
            directory[length - 1U] = '\0';
        }
        mkdir(directory, 0775);

        char path[224]{};
        char temporary[224]{};
        runtime->savePath(path, sizeof(path), "pdsnake.save");
        runtime->savePath(temporary, sizeof(temporary), "pdsnake.tmp");
        FILE* file = std::fopen(temporary, "wb");
        if (!file) {
            lua_pushboolean(state, 0);
            return 1;
        }

        lua_pushnil(state);
        while (lua_next(state, 1) != 0) {
            if (lua_type(state, -2) == LUA_TSTRING) {
                const char* key = lua_tostring(state, -2);
                const int type = lua_type(state, -1);
                if (type == LUA_TBOOLEAN) {
                    std::fprintf(file, "%s=b:%d\n", key,
                                 lua_toboolean(state, -1) ? 1 : 0);
                } else if (type == LUA_TNUMBER) {
                    std::fprintf(file, "%s=n:%.17g\n", key,
                                 lua_tonumber(state, -1));
                } else if (type == LUA_TSTRING) {
                    std::fprintf(file, "%s=s:%s\n", key,
                                 lua_tostring(state, -1));
                }
            }
            lua_pop(state, 1);
        }

        bool wrote = std::fclose(file) == 0;
        if (wrote && std::rename(temporary, path) != 0) {
            std::remove(path);
            wrote = std::rename(temporary, path) == 0;
        }
        if (!wrote) std::remove(temporary);
        lua_pushboolean(state, wrote);
        return 1;
    }

    static int cGetSystemMenu(lua_State* state) {
        Impl* runtime = self(state);
        lua_newtable(state);
        runtime->setFunction(-1, "addCheckmarkMenuItem",
                             cAddCheckmarkMenuItem);
        return 1;
    }

    static int cAddCheckmarkMenuItem(lua_State*) {
        // Pogopo's physical Power-button quick menu remains authoritative.
        // PDSnake only registers optional settings here and does not require
        // the returned menu item.
        return 0;
    }

    void createMetatables() {
        if (luaL_newmetatable(lua, kFontMetatable)) {
            lua_pushvalue(lua, -1);
            lua_setfield(lua, -2, "__index");
        }
        lua_pop(lua, 1);

        if (luaL_newmetatable(lua, kImageMetatable)) {
            lua_pushvalue(lua, -1);
            lua_setfield(lua, -2, "__index");
            setFunction(-1, "draw", cImageDraw);
            setFunction(-1, "getSize", cImageGetSize);
        }
        lua_pop(lua, 1);

        if (luaL_newmetatable(lua, kSoundMetatable)) {
            lua_pushvalue(lua, -1);
            lua_setfield(lua, -2, "__index");
            setFunction(-1, "play", cSoundPlay);
            setFunction(-1, "setVolume", cSoundSetVolume);
        }
        lua_pop(lua, 1);
    }

    void createTimerRegistry() {
        lua_newtable(lua);
        lua_newtable(lua);
        lua_pushliteral(lua, "v");
        lua_setfield(lua, -2, "__mode");
        lua_setmetatable(lua, -2);
        lua_rawsetp(lua, LUA_REGISTRYINDEX, &kTimerRegistryKey);
    }

    void registerApi() {
        createMetatables();
        createTimerRegistry();

        pushFunction(cImport);
        lua_setglobal(lua, "import");

        lua_newtable(lua);
        const int playdate = lua_gettop(lua);

        lua_pushinteger(lua, 0x04);
        lua_setfield(lua, playdate, "kButtonLeft");
        lua_pushinteger(lua, 0x08);
        lua_setfield(lua, playdate, "kButtonRight");
        lua_pushinteger(lua, 0x01);
        lua_setfield(lua, playdate, "kButtonUp");
        lua_pushinteger(lua, 0x02);
        lua_setfield(lua, playdate, "kButtonDown");
        lua_pushinteger(lua, 0x20);
        lua_setfield(lua, playdate, "kButtonA");
        lua_pushinteger(lua, 0x10);
        lua_setfield(lua, playdate, "kButtonB");

        setFunction(playdate, "getCurrentTimeMilliseconds", cGetCurrentTime);
        setFunction(playdate, "buttonJustPressed", cButtonJustPressed);
        setFunction(playdate, "buttonIsPressed", cButtonIsPressed);
        setFunction(playdate, "getSystemMenu", cGetSystemMenu);
        setFunction(playdate, "setMenuImage", cSetMenuImage);
        setFunction(playdate, "drawFPS", cDrawFps);

        lua_newtable(lua);
        lua_pushliteral(lua, "PDSnake");
        lua_setfield(lua, -2, "name");
        lua_pushliteral(lua, "1.2");
        lua_setfield(lua, -2, "version");
        lua_pushliteral(lua, "7");
        lua_setfield(lua, -2, "buildNumber");
        lua_pushliteral(lua, "Brett Chalupa");
        lua_setfield(lua, -2, "author");
        lua_setfield(lua, playdate, "metadata");

        lua_newtable(lua);
        setFunction(-1, "getWidth", cDisplayWidth);
        setFunction(-1, "getHeight", cDisplayHeight);
        setFunction(-1, "setRefreshRate", cSetRefreshRate);
        setFunction(-1, "setInverted", cSetInverted);
        lua_setfield(lua, playdate, "display");

        lua_newtable(lua);
        const int graphics = lua_gettop(lua);
        lua_pushinteger(lua, 0);
        lua_setfield(lua, graphics, "kColorBlack");
        lua_pushinteger(lua, 1);
        lua_setfield(lua, graphics, "kColorWhite");
        setFunction(graphics, "clear", cGraphicsClear);
        setFunction(graphics, "setColor", cSetColor);
        setFunction(graphics, "setLineWidth", cSetLineWidth);
        setFunction(graphics, "fillRect", cFillRect);
        setFunction(graphics, "fillCircleAtPoint", cFillCircle);
        setFunction(graphics, "drawCircleInRect", cDrawCircleInRect);
        setFunction(graphics, "drawText", cDrawText);
        setFunction(graphics, "setFont", cSetFont);
        setFunction(graphics, "pushContext", cPushContext);
        setFunction(graphics, "popContext", cPopContext);

        lua_newtable(lua);
        setFunction(-1, "new", cFontNew);
        lua_setfield(lua, graphics, "font");
        lua_newtable(lua);
        setFunction(-1, "new", cImageNew);
        lua_setfield(lua, graphics, "image");
        lua_setfield(lua, playdate, "graphics");

        lua_newtable(lua);
        lua_newtable(lua);
        setFunction(-1, "new", cSoundNew);
        lua_setfield(lua, -2, "sampleplayer");
        lua_setfield(lua, playdate, "sound");

        lua_newtable(lua);
        setFunction(-1, "new", cTimerNew);
        setFunction(-1, "updateTimers", cUpdateTimers);
        lua_setfield(lua, playdate, "timer");

        lua_newtable(lua);
        setFunction(-1, "read", cDatastoreRead);
        setFunction(-1, "write", cDatastoreWrite);
        lua_setfield(lua, playdate, "datastore");

        lua_setglobal(lua, "playdate");
    }

    void seedRandom() {
        const int top = lua_gettop(lua);
        lua_getglobal(lua, "math");
        if (lua_istable(lua, -1)) {
            lua_getfield(lua, -1, "randomseed");
            if (lua_isfunction(lua, -1)) {
                lua_pushinteger(lua, static_cast<lua_Integer>(
                    esp_timer_get_time() & 0x7FFFFFFF));
                if (lua_pcall(lua, 1, 0, 0) != LUA_OK) lua_pop(lua, 1);
            } else {
                lua_pop(lua, 1);
            }
        }
        lua_settop(lua, top);
    }

    esp_err_t start(gfx::Canvas& target_canvas, audio::Audio& target_audio,
                    storage::Storage& target_storage) {
        canvas = &target_canvas;
        audio = &target_audio;
        storage = &target_storage;
        last_error[0] = '\0';
        loaded_modules.fill(false);
        runtime_stats = {};
        runtime_stats.requested_fps = kDefaultFps;
        refresh_rate = kDefaultFps;
        frame_accumulator_ms = 0;
        now_ms = 0;
        pressed_buttons = 0;
        held_buttons = 0;
        next_timer_id = 1;

        lua = lua_newstate(allocator, this);
        if (!lua) {
            setError("startup", "could not allocate Lua state");
            return ESP_ERR_NO_MEM;
        }
        luaL_openlibs(lua);
        registerApi();
        seedRandom();

        if (!importModule("main")) {
            lua_close(lua);
            lua = nullptr;
            return ESP_FAIL;
        }

        is_running = true;
        ESP_LOGI(TAG,
                 "PogoDate Lite ready: PDSnake source Lua 5.4, 400x240, "
                 "logic=%lu FPS LCD cap=30",
                 static_cast<unsigned long>(refresh_rate));
        return ESP_OK;
    }

    void stop() {
        if (lua && is_running) {
            callGlobal("playdate", "gameWillTerminate", true);
        }
        is_running = false;
        if (lua) {
            lua_close(lua);
            lua = nullptr;
        }
        canvas = nullptr;
        audio = nullptr;
        storage = nullptr;
    }

    uint32_t update(uint32_t dt_ms) {
        if (!lua || !is_running) return 0;
        frame_accumulator_ms =
            std::min<uint32_t>(frame_accumulator_ms + dt_ms, 250U);
        now_ms += dt_ms;

        const uint32_t interval =
            std::max<uint32_t>(1U, 1000U / std::max<uint32_t>(1U, refresh_rate));
        uint32_t produced = 0;
        while (frame_accumulator_ms >= interval && produced < 3U) {
            frame_accumulator_ms -= interval;
            frame_dt_ms = interval;
            const int64_t started = esp_timer_get_time();
            if (!callGlobal("playdate", "update")) {
                is_running = false;
                break;
            }
            const uint32_t update_us = static_cast<uint32_t>(
                std::max<int64_t>(0, esp_timer_get_time() - started));
            runtime_stats.last_update_us = update_us;
            runtime_stats.max_update_us =
                std::max(runtime_stats.max_update_us, update_us);
            ++runtime_stats.lua_frames;
            ++produced;
            pressed_buttons = 0;
        }

        runtime_stats.lua_bytes = allocated_bytes;
        runtime_stats.lua_peak_bytes = peak_allocated_bytes;
        if (lua) {
            runtime_stats.lua_gc_bytes =
                static_cast<size_t>(lua_gc(lua, LUA_GCCOUNT)) * 1024U +
                static_cast<size_t>(lua_gc(lua, LUA_GCCOUNTB));
        }
        return produced;
    }
};

Runtime::Runtime() : impl_(new (std::nothrow) Impl) {}

Runtime::~Runtime() {
    stop();
    delete impl_;
    impl_ = nullptr;
}

esp_err_t Runtime::start(gfx::Canvas& canvas, audio::Audio& audio,
                         storage::Storage& storage) {
    if (!impl_) return ESP_ERR_NO_MEM;
    impl_->stop();
    return impl_->start(canvas, audio, storage);
}

void Runtime::stop() {
    if (impl_) impl_->stop();
}

void Runtime::setInput(uint8_t held_mask, uint8_t pressed_mask) {
    if (!impl_) return;
    impl_->held_buttons = held_mask;
    impl_->pressed_buttons =
        static_cast<uint8_t>(impl_->pressed_buttons | pressed_mask);
}

uint32_t Runtime::update(uint32_t dt_ms) {
    return impl_ ? impl_->update(dt_ms) : 0;
}

bool Runtime::running() const {
    return impl_ && impl_->is_running;
}

const char* Runtime::error() const {
    return impl_ && impl_->last_error[0] ? impl_->last_error : "";
}

Stats Runtime::stats() const {
    return impl_ ? impl_->runtime_stats : Stats{};
}

} // namespace pogopo::playdate
