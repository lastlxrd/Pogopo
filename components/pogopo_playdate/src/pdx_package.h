#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace pogopo::playdate {

struct PdzEntry {
    char name[128]{};
    uint32_t data_offset = 0;
    uint32_t stored_size = 0;
    uint32_t unpacked_size = 0;
    uint8_t type = 0;
    uint8_t slot = 0;
    bool compressed = false;
};

class PdzArchive {
public:
    static constexpr size_t MAX_ENTRIES = 96;

    esp_err_t open(const char* path, char* error, size_t error_capacity,
                   bool require_main = true);
    void close();
    const PdzEntry* findLua(const char* module) const;
    // Caller owns the returned heap_caps_malloc()-compatible buffer.
    esp_err_t load(const PdzEntry& entry, uint8_t*& data, size_t& size,
                   char* error, size_t error_capacity) const;

    const char* path() const { return path_; }
    size_t count() const { return count_; }
    const PdzEntry* entry(size_t index) const {
        return index < count_ ? &entries_[index] : nullptr;
    }
    size_t luaCount() const;

private:
    std::array<PdzEntry, MAX_ENTRIES> entries_{};
    size_t count_ = 0;
    char path_[224]{};
};

bool pdxJoinPath(char* out, size_t capacity, const char* root,
                 const char* relative, const char* extension = nullptr);

// Playdate keeps the Lua 5.4-beta opcode order for package compatibility.
// Rewrites every instruction in a mutable 32-bit Playdate Lua chunk to the
// stock Lua 5.4 opcode order used by the ESP-IDF component.
bool normalizePlaydateLuaBytecode(uint8_t* data, size_t size,
                                  char* error, size_t error_capacity);

} // namespace pogopo::playdate
