#pragma once

// gpSP uses RETRO_GO only to move its largest state buffers out of static
// internal RAM. pogopoOS supplies its own frontend, so the full Retro-Go
// platform header is intentionally not pulled in here.
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#endif
