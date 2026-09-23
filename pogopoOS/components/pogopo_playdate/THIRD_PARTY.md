# Third-party code

## Lua 5.4.7

The runtime uses the `georgik/lua` ESP-IDF component, version 5.4.7, from the
Espressif Component Registry. Lua is Copyright (C) 1994-2024 Lua.org, PUC-Rio,
and is distributed under the MIT license.

## PDSnake

The unmodified PDSnake Lua sources in `game/pdsnake/source` are by Brett
Chalupa and are dedicated to the public domain under the Unlicense. The full
license text is preserved at `third_party/pdsnake/LICENSE.txt`.

## Celeste Classic 1.0.3 for Playdate

The original Lua sources and assets in `game/celeste/source` are from the
Playdate port by Rémi Parmentier (HTeuMeuLeu), version 1.0.3. Celeste Classic
was created by Noel Berry and Maddy Thorson. PICO-8 and the PICO-8 font are the
property of Lexaloffle Games. The port is distributed under CC BY-NC-SA 4.0;
the complete license text is preserved at `game/celeste/LICENSE.txt`.

Pogopo adds a technical compatibility layer and packs the original bitmap/font
data into `src/celeste_assets.cpp`; the readable upstream assets remain next to
the Lua source. This build is for non-commercial testing and remains subject to
the same attribution, non-commercial and share-alike terms.

Pogopo's Playdate API compatibility layer, input mapping, Sharp LCD drawing,
native sound mapping, source loader and datastore backend are project-specific
code.
