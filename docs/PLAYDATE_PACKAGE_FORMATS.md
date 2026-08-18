# Playdate package formats used by PogoDate

These notes describe the practical loader implemented in STEP11.2 and the
Celeste Classic 1.0.3 package used to validate it. They are not a promise that
undocumented binary details will remain identical in every future SDK.

## PDX: the game bundle

A `.pdx` is a directory. `pdc Source Game.pdx` compiles source code and assets
into that directory. The important entries are:

- `pdxinfo`: UTF-8 `key=value` metadata;
- `main.pdz`: compiled Lua modules;
- `pdex.bin`: optional native Playdate ARM executable, used instead of Lua;
- `.pdi`: compiled image;
- `.pdt`: compiled image table;
- `.pft`: compiled font;
- `.pda`: compiled audio.

A download may wrap the directory in ZIP for transport. ZIP is not the runtime
format. STEP11.2 expects that wrapper to be extracted on a PC first.

## How the PDZ loader works

`main.pdz` begins with the 16-byte `Playdate PDZ` signature. PogoDate then
walks records sequentially:

1. Read a little-endian 32-bit record word. Its low 7 bits are the resource
   type, bit 7 marks zlib compression, and the upper 24 bits are block length.
2. Read the NUL-terminated module name and advance to a four-byte boundary.
3. For a compressed record, read a little-endian 32-bit unpacked length and
   inflate the remaining block with zlib.
4. Type 1 is Lua bytecode; types 2 and 3 are image/image-table records.
5. `import "Scripts/Game"` looks up that exact record and recursively rewrites
   Playdate's opcode numbering to stock Lua 5.4 numbering.
6. The normalized bytes are passed to `luaL_loadbufferx(..., "b")`.

The Celeste bytecode has the normal Lua 5.4 signature and 4-byte instruction,
integer and number fields. That matches the firmware's `LUA_32BITS` build, but
the header does not describe the opcode table. Playdate uses a tweaked Lua
5.4.3 runtime and retains the Lua 5.4-beta opcode order for compatibility. Its
`LOADFALSE`, `LFALSESKIP`, and `LOADTRUE` opcodes are appended after
`EXTRAARG`, shifting most other opcode numbers down by two. Passing that chunk
directly to stock Lua loads successfully but executes the wrong instructions.
The mapping is documented by the community reverse-engineering project:
<https://github.com/cranksters/playdate-reverse-engineering/blob/main/formats/luac.md>.

Malformed lengths, overlong names, missing `main`, unknown beta opcodes,
unsupported constant tags, path traversal and records outside the file are
rejected before Lua executes the chunk.

Every translation unit that calls the Lua C API must see the same 32-bit Lua
configuration as the linked interpreter. Mixing a `LUA_32BITS=1` library with
default 64-bit `lua_Number`/`lua_Integer` headers is an ABI error: a chunk can
appear to start while coordinates and timing values are corrupted at native
function calls. The firmware's managed Lua component supplies the 32-bit
configuration; host tests must use the matching headers as well.

## How PDI and PDT become pixels

Both formats use a 32-byte outer header followed by zlib data. The outer header
contains the full image/cell dimensions. An unpacked image record contains a
16-byte descriptor followed by row-major, MSB-first 1-bit pixels. Transparent
images carry a second 1-bit mask plane; trimmed transparent borders are restored
using the descriptor offsets.

A PDT starts with its frame count and a table of cumulative frame offsets.
PogoDate validates every offset, decodes a frame when Lua first requests it,
and then keeps that Lua image in the image table's cache.

## How PFT fonts become glyphs

PFT uses the same 32-byte compressed-resource wrapper and begins its unpacked
data with maximum glyph width, maximum glyph height, tracking and a 512-page
presence bitmap. Cumulative page offsets lead to per-page 256-glyph presence
maps and cumulative glyph offsets. Each glyph stores its advance, short/long
kerning pairs and a PDI-style bitmap cell with optional transparency mask.

PogoDate resolves UTF-8 codepoints through those packed maps, applies tracking
and kerning against the next character, restores trimmed cell offsets and
draws the original 1-bit pixels. The selected font userdata is retained in the
Lua registry so `graphics.setFont(font)` remains valid after the caller's local
reference leaves scope. Presence-map popcounts avoid scanning every possible
page and glyph for each character.

## How PDA audio becomes I2S PCM

PDA starts with the 16-byte `Playdate AUD` header. Bytes 12-14 contain the
little-endian 24-bit sample rate and byte 15 identifies the sample format:

- `0`: unsigned 8-bit mono PCM;
- `1`: unsigned 8-bit stereo PCM;
- `2`: signed 16-bit mono PCM;
- `3`: signed 16-bit stereo PCM;
- `4`: mono IMA ADPCM. The payload starts with a little-endian block size,
  followed by ordinary IMA blocks (predictor, step index, reserved byte,
  low-nibble-first samples);
- `5`: stereo IMA ADPCM, not implemented by this step.

Pogopo's speaker path is mono, so stereo PCM is averaged to mono while reading
the file in small chunks. This avoids holding both the original stereo stream
and the complete converted stream in PSRAM. The decoded sample count is capped
before allocation and malformed frame/block boundaries are rejected.

Music is decoded once into PSRAM and loops in its own PCM mixer voice. Effects
use the independent one-shot PCM voice.

## Why pdex.bin is different

`pdex.bin` is executable machine code for Playdate's ARM Cortex-M7 and calls
the SDK through a Playdate API table. The ESP32-S3 uses Xtensa LX7 instructions,
a different ABI and different memory/peripheral layout. Loading the file does
not translate it: the first ARM instruction would simply be meaningless to the
Xtensa CPU.

There are only three realistic paths for a native game:

1. rebuild its source for ESP32-S3 against a compatibility layer;
2. manually port/rewrite the platform-dependent parts;
3. implement an ARM emulator/dynamic translator and essentially the full
   Playdate runtime, which is generally too costly for full-speed games here.

That architecture difference is why STEP11.2 launches Lua `main.pdz` packages
but only identifies and explains `pdex.bin` packages.
