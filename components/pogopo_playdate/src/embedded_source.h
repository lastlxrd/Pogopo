#pragma once

#include <cstddef>

namespace pogopo::playdate {

struct EmbeddedSource {
    const char* name;
    const char* data;
    size_t size;
};

const EmbeddedSource* pdsnakeSources(size_t& count);

} // namespace pogopo::playdate
