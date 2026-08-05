#include "pdx_package.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <new>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "pogopo/playdate/runtime.h"
#include "zlib.h"

namespace pogopo::playdate {
namespace {

constexpr uint32_t kMaximumPdzBlock = 2U * 1024U * 1024U;

uint32_t le32(const uint8_t* value) {
    return static_cast<uint32_t>(value[0]) |
           (static_cast<uint32_t>(value[1]) << 8U) |
           (static_cast<uint32_t>(value[2]) << 16U) |
           (static_cast<uint32_t>(value[3]) << 24U);
}

void setError(char* out, size_t capacity, const char* text) {
    if (!out || capacity == 0) return;
    std::snprintf(out, capacity, "%s", text ? text : "unknown package error");
}

void copyText(char* out, size_t capacity, const char* value) {
    if (!out || capacity == 0) return;
    std::snprintf(out, capacity, "%s", value ? value : "");
}

PdzArchive* allocateArchive() {
    void* memory = heap_caps_malloc(
        sizeof(PdzArchive), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) {
        memory = heap_caps_malloc(sizeof(PdzArchive), MALLOC_CAP_8BIT);
    }
    return memory ? new (memory) PdzArchive() : nullptr;
}

void releaseArchive(PdzArchive* archive) {
    if (!archive) return;
    archive->~PdzArchive();
    heap_caps_free(archive);
}

bool regularFile(const char* path) {
    struct stat value{};
    return path && stat(path, &value) == 0 && S_ISREG(value.st_mode);
}

bool endsWith(const char* value, const char* suffix) {
    if (!value || !suffix) return false;
    const size_t length = std::strlen(value);
    const size_t suffix_length = std::strlen(suffix);
    if (length < suffix_length) return false;
    value += length - suffix_length;
    for (size_t i = 0; i < suffix_length; ++i) {
        char a = value[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
        if (a != b) return false;
    }
    return true;
}

void countAssets(const char* path, uint16_t& images, uint16_t& audio,
                 int depth = 0) {
    if (!path || depth > 5) return;
    DIR* directory = opendir(path);
    if (!directory) return;
    while (dirent* entry = readdir(directory)) {
        if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) {
            continue;
        }
        char child[256]{};
        if (!pdxJoinPath(child, sizeof(child), path, entry->d_name)) continue;
        struct stat value{};
        if (stat(child, &value) != 0) continue;
        if (S_ISDIR(value.st_mode)) {
            countAssets(child, images, audio, depth + 1);
        } else if (S_ISREG(value.st_mode)) {
            if (endsWith(entry->d_name, ".pdi") || endsWith(entry->d_name, ".pdt") ||
                endsWith(entry->d_name, ".pft")) {
                if (images != UINT16_MAX) ++images;
            } else if (endsWith(entry->d_name, ".pda")) {
                if (audio != UINT16_MAX) ++audio;
            }
        }
    }
    closedir(directory);
}

void readPdxInfo(const char* root, PackageInfo& info) {
    char path[224]{};
    if (!pdxJoinPath(path, sizeof(path), root, "pdxinfo")) return;
    FILE* file = std::fopen(path, "rb");
    if (!file) return;
    char line[256]{};
    while (std::fgets(line, sizeof(line), file)) {
        char* newline = std::strpbrk(line, "\r\n");
        if (newline) *newline = '\0';
        char* separator = std::strchr(line, '=');
        if (!separator) continue;
        *separator++ = '\0';
        if (!std::strcmp(line, "name")) copyText(info.name, sizeof(info.name), separator);
        else if (!std::strcmp(line, "author")) copyText(info.author, sizeof(info.author), separator);
        else if (!std::strcmp(line, "version")) copyText(info.version, sizeof(info.version), separator);
        else if (!std::strcmp(line, "bundleID")) copyText(info.bundle_id, sizeof(info.bundle_id), separator);
    }
    std::fclose(file);
}

} // namespace

bool pdxJoinPath(char* out, size_t capacity, const char* root,
                 const char* relative, const char* extension) {
    if (!out || capacity == 0 || !root || !relative) return false;
    while (*relative == '/') ++relative;
    const bool slash = root[0] && root[std::strlen(root) - 1U] != '/';
    const int count = std::snprintf(out, capacity, "%s%s%s%s", root,
                                    slash ? "/" : "", relative,
                                    extension ? extension : "");
    return count > 0 && static_cast<size_t>(count) < capacity;
}

esp_err_t PdzArchive::open(const char* path, char* error, size_t error_capacity) {
    close();
    if (!path || std::strlen(path) >= sizeof(path_)) {
        setError(error, error_capacity, "main.pdz path is too long");
        return ESP_ERR_INVALID_ARG;
    }
    FILE* file = std::fopen(path, "rb");
    if (!file) {
        setError(error, error_capacity, "main.pdz could not be opened");
        return ESP_ERR_NOT_FOUND;
    }
    std::fseek(file, 0, SEEK_END);
    const long file_length = std::ftell(file);
    std::rewind(file);
    uint8_t header[16]{};
    if (file_length < 16 || std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
        std::memcmp(header, "Playdate PDZ", 12) != 0) {
        std::fclose(file);
        setError(error, error_capacity, "invalid Playdate PDZ header");
        return ESP_ERR_INVALID_RESPONSE;
    }

    while (std::ftell(file) < file_length) {
        if (count_ >= entries_.size()) {
            std::fclose(file); close();
            setError(error, error_capacity, "main.pdz has too many records");
            return ESP_ERR_NO_MEM;
        }
        uint8_t word_bytes[4]{};
        if (std::fread(word_bytes, 1, 4, file) != 4) {
            std::fclose(file); close();
            setError(error, error_capacity, "truncated PDZ record header");
            return ESP_ERR_INVALID_SIZE;
        }
        const uint32_t word = le32(word_bytes);
        const uint32_t block_size = word >> 8U;
        PdzEntry entry{};
        entry.type = static_cast<uint8_t>(word & 0x7FU);
        entry.compressed = (word & 0x80U) != 0;
        if (block_size == 0 || block_size > kMaximumPdzBlock ||
            (entry.compressed && block_size < 4U)) {
            std::fclose(file); close();
            setError(error, error_capacity, "invalid PDZ record size");
            return ESP_ERR_INVALID_SIZE;
        }

        size_t name_length = 0;
        for (;;) {
            const int value = std::fgetc(file);
            if (value == EOF || name_length + 1U >= sizeof(entry.name)) {
                std::fclose(file); close();
                setError(error, error_capacity, "invalid PDZ module name");
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (value == 0) break;
            entry.name[name_length++] = static_cast<char>(value);
        }
        entry.name[name_length] = '\0';
        long position = std::ftell(file);
        position = (position + 3L) & ~3L;
        if (std::fseek(file, position, SEEK_SET) != 0) {
            std::fclose(file); close();
            setError(error, error_capacity, "invalid PDZ name alignment");
            return ESP_ERR_INVALID_RESPONSE;
        }

        entry.unpacked_size = block_size;
        entry.stored_size = block_size;
        if (entry.compressed) {
            uint8_t unpacked[4]{};
            if (std::fread(unpacked, 1, 4, file) != 4) {
                std::fclose(file); close();
                setError(error, error_capacity, "truncated PDZ compressed record");
                return ESP_ERR_INVALID_SIZE;
            }
            entry.unpacked_size = le32(unpacked);
            entry.stored_size = block_size - 4U;
        }
        entry.data_offset = static_cast<uint32_t>(std::ftell(file));
        const uint64_t end = static_cast<uint64_t>(entry.data_offset) + entry.stored_size;
        if (entry.unpacked_size == 0 || entry.unpacked_size > kMaximumPdzBlock ||
            end > static_cast<uint64_t>(file_length) ||
            std::fseek(file, static_cast<long>(end), SEEK_SET) != 0) {
            std::fclose(file); close();
            setError(error, error_capacity, "PDZ record runs past end of file");
            return ESP_ERR_INVALID_SIZE;
        }
        entry.slot = static_cast<uint8_t>(count_);
        entries_[count_++] = entry;
    }
    std::fclose(file);
    if (!findLua("main")) {
        close();
        setError(error, error_capacity, "main.pdz does not contain Lua module main");
        return ESP_ERR_NOT_FOUND;
    }
    copyText(path_, sizeof(path_), path);
    return ESP_OK;
}

void PdzArchive::close() {
    count_ = 0;
    path_[0] = '\0';
    for (auto& entry : entries_) entry = {};
}

const PdzEntry* PdzArchive::findLua(const char* module) const {
    if (!module || !module[0]) return nullptr;
    char normalized[128]{};
    copyText(normalized, sizeof(normalized), module);
    const size_t length = std::strlen(normalized);
    if (length > 4U && !std::strcmp(normalized + length - 4U, ".lua")) {
        normalized[length - 4U] = '\0';
    }
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].type == 1U && !std::strcmp(entries_[i].name, normalized)) {
            return &entries_[i];
        }
    }
    return nullptr;
}

esp_err_t PdzArchive::load(const PdzEntry& entry, uint8_t*& data, size_t& size,
                           char* error, size_t error_capacity) const {
    data = nullptr;
    size = 0;
    if (!path_[0] || entry.unpacked_size == 0 || entry.unpacked_size > kMaximumPdzBlock) {
        setError(error, error_capacity, "invalid PDZ load request");
        return ESP_ERR_INVALID_ARG;
    }
    FILE* file = std::fopen(path_, "rb");
    if (!file || std::fseek(file, static_cast<long>(entry.data_offset), SEEK_SET) != 0) {
        if (file) std::fclose(file);
        setError(error, error_capacity, "could not seek main.pdz");
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t* output = static_cast<uint8_t*>(heap_caps_malloc(
        entry.unpacked_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!output) output = static_cast<uint8_t*>(heap_caps_malloc(
        entry.unpacked_size, MALLOC_CAP_8BIT));
    if (!output) {
        std::fclose(file);
        setError(error, error_capacity, "not enough memory for PDZ module");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_OK;
    if (!entry.compressed) {
        if (std::fread(output, 1, entry.stored_size, file) != entry.stored_size) {
            result = ESP_ERR_INVALID_SIZE;
        }
    } else {
        uint8_t* packed = static_cast<uint8_t*>(heap_caps_malloc(
            entry.stored_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!packed) packed = static_cast<uint8_t*>(heap_caps_malloc(
            entry.stored_size, MALLOC_CAP_8BIT));
        if (!packed) {
            result = ESP_ERR_NO_MEM;
        } else {
            if (std::fread(packed, 1, entry.stored_size, file) != entry.stored_size) {
                result = ESP_ERR_INVALID_SIZE;
            } else {
                uLongf unpacked = entry.unpacked_size;
                const int zerr = uncompress(output, &unpacked, packed, entry.stored_size);
                if (zerr != Z_OK || unpacked != entry.unpacked_size) {
                    result = ESP_ERR_INVALID_RESPONSE;
                }
            }
            heap_caps_free(packed);
        }
    }
    std::fclose(file);
    if (result != ESP_OK) {
        heap_caps_free(output);
        setError(error, error_capacity, "PDZ decompression/read failed");
        return result;
    }
    data = output;
    size = entry.unpacked_size;
    return ESP_OK;
}

size_t PdzArchive::luaCount() const {
    size_t result = 0;
    for (size_t i = 0; i < count_; ++i) if (entries_[i].type == 1U) ++result;
    return result;
}

esp_err_t inspectPackage(const char* pdx_path, PackageInfo& info,
                         bool count_assets) {
    info = {};
    if (!pdx_path || !pdx_path[0] || std::strlen(pdx_path) >= sizeof(info.path)) {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat root{};
    if (stat(pdx_path, &root) != 0 || !S_ISDIR(root.st_mode)) return ESP_ERR_NOT_FOUND;
    copyText(info.path, sizeof(info.path), pdx_path);
    const char* basename = std::strrchr(pdx_path, '/');
    copyText(info.name, sizeof(info.name), basename ? basename + 1 : pdx_path);
    readPdxInfo(pdx_path, info);
    if (count_assets) {
        countAssets(pdx_path, info.image_files, info.audio_files);
    }

    char path[224]{};
    if (pdxJoinPath(path, sizeof(path), pdx_path, "pdex.bin") && regularFile(path)) {
        info.kind = PackageKind::NativeBinary;
        return ESP_OK;
    }
    if (!pdxJoinPath(path, sizeof(path), pdx_path, "main.pdz") || !regularFile(path)) {
        return ESP_ERR_NOT_FOUND;
    }
    // PdzArchive is roughly 14 KiB because it stores 96 record descriptors.
    // The pogopo_os UI task has an 8 KiB stack, so a local archive here
    // corrupted that stack as soon as the launcher found a real .pdx folder.
    // Keep the index in PSRAM/heap just like Runtime::Impl does.
    PdzArchive* archive = allocateArchive();
    if (!archive) return ESP_ERR_NO_MEM;
    char error[96]{};
    const esp_err_t result = archive->open(path, error, sizeof(error));
    if (result != ESP_OK) {
        releaseArchive(archive);
        return result;
    }
    info.kind = PackageKind::LuaPdz;
    info.lua_modules = static_cast<uint16_t>(
        std::min<size_t>(UINT16_MAX, archive->luaCount()));
    releaseArchive(archive);
    return ESP_OK;
}

const char* packageKindName(PackageKind kind) {
    switch (kind) {
        case PackageKind::LuaPdz: return "LUA PDZ";
        case PackageKind::NativeBinary: return "ARM PDEX.BIN";
        default: return "INVALID";
    }
}

} // namespace pogopo::playdate
