# Third-party software: gpSP

This experimental component contains the gpSP core from Retro-Go development
commit `fa7ea7839c978b01cb39f541688d03e169afca2b` (2026-01-19).

- Upstream: https://github.com/ducalex/retro-go/tree/dev/gbsp
- Core origin: gameplaySP / gpSP
- License: GNU General Public License, version 2 or later
- Included license text: `third_party/gpsp/COPYING`

pogopoOS-specific changes are limited to the ESP-IDF frontend, dynamic PSRAM
state, SD-backed ROM paging diagnostics, safe partial-page reads, and build
configuration for the ESP32-S3 interpreter path. No proprietary Nintendo BIOS
or game ROM is included. The bundled open BIOS is the upstream Normatt-derived
open replacement already distributed by the gpSP project.
