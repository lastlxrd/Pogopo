#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "pogopo/gfx/canvas.h"
#include "pogopo_audio.h"
#include "pogopo_storage.h"

namespace pogopo::playdate {

enum class Game : uint8_t {
    PDSnake,
    Celeste,
    External,
};

enum class PackageKind : uint8_t {
    Invalid,
    LuaPdz,
    NativeBinary,
};

struct PackageInfo {
    char path[192]{};
    char name[64]{};
    char author[64]{};
    char version[24]{};
    char bundle_id[80]{};
    PackageKind kind = PackageKind::Invalid;
    uint16_t lua_modules = 0;
    uint16_t image_files = 0;
    uint16_t audio_files = 0;
};

// Reads pdxinfo and validates main.pdz/pdex.bin without executing the package.
// Asset counting recursively walks the whole PDX and is intentionally opt-in;
// the launcher only needs a quick validation pass.
esp_err_t inspectPackage(const char* pdx_path, PackageInfo& info,
                         bool count_assets = false);
const char* packageKindName(PackageKind kind);

struct Stats {
    uint32_t lua_frames = 0;
    uint32_t last_update_us = 0;
    uint32_t max_update_us = 0;
    uint32_t last_logic_us = 0;
    uint32_t last_blit_us = 0;
    uint32_t errors = 0;
    uint32_t requested_fps = 50;
    size_t lua_bytes = 0;
    size_t lua_peak_bytes = 0;
    size_t lua_gc_bytes = 0;
};

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    esp_err_t start(gfx::Canvas& canvas, audio::Audio& audio,
                    storage::Storage& storage,
                    Game game = Game::PDSnake);
    esp_err_t startPackage(gfx::Canvas& canvas, audio::Audio& audio,
                           storage::Storage& storage,
                           const char* pdx_path);
    void stop();

    void setInput(uint8_t held_mask, uint8_t pressed_mask);
    // Supplies the latest normalized accelerometer sample in g. Games opt in
    // through playdate.startAccelerometer()/readAccelerometer().
    void setAccelerometer(float x, float y, float z, bool valid = true);
    // Advances the native Lua VM. Returns the number of Playdate frames run.
    uint32_t update(uint32_t dt_ms);

    bool running() const;
    const char* error() const;
    Stats stats() const;

#ifdef PD_HOST_TEST
    // Host-only regression hook. Firmware builds do not expose arbitrary Lua
    // evaluation; native tests use this to enter otherwise distant scenes.
    bool evalForTest(const char* source);
#endif

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace pogopo::playdate
