#!/usr/bin/env python3
"""Pack Pogopo menu GIFs and OTF fonts into one linker-friendly binary blob."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageSequence


ASCII_FIRST = 0x20
ASCII_LAST = 0x7E


@dataclass
class Animation:
    name: str
    offset: int
    width: int
    height: int
    stride: int
    frame_count: int
    source_x: int
    source_y: int
    durations: list[int]


@dataclass
class Glyph:
    offset: int
    width: int
    height: int
    x_offset: int
    advance: int


@dataclass
class Font:
    name: str
    line_height: int
    glyphs: list[Glyph]


def black_mask(frame: Image.Image) -> Image.Image:
    rgba = frame.convert("RGBA")
    pixels = rgba.load()
    result = Image.new("1", rgba.size, 0)
    out = result.load()
    for y in range(rgba.height):
        for x in range(rgba.width):
            red, green, blue, alpha = pixels[x, y]
            out[x, y] = alpha >= 128 and (red + green + blue) < 384
    return result


def pack_mask(mask: Image.Image) -> bytes:
    width, height = mask.size
    stride = (width + 7) // 8
    packed = bytearray(stride * height)
    pixels = mask.load()
    for y in range(height):
        for x in range(width):
            if pixels[x, y]:
                packed[y * stride + (x >> 3)] |= 0x80 >> (x & 7)
    return bytes(packed)


def append_animation(blob: bytearray, name: str, path: Path) -> Animation:
    with Image.open(path) as source:
        frames: list[Image.Image] = []
        durations: list[int] = []
        for frame in ImageSequence.Iterator(source):
            # Pillow exposes each GIF frame composited onto the logical canvas.
            frames.append(black_mask(frame.copy()))
            durations.append(max(10, int(frame.info.get("duration", 100))))

    union: tuple[int, int, int, int] | None = None
    for frame in frames:
        bbox = frame.getbbox()
        if not bbox:
            continue
        if union is None:
            union = bbox
        else:
            union = (
                min(union[0], bbox[0]), min(union[1], bbox[1]),
                max(union[2], bbox[2]), max(union[3], bbox[3]),
            )
    if union is None:
        raise RuntimeError(f"{path} contains no black pixels")

    left, top, right, bottom = union
    width = right - left
    height = bottom - top
    stride = (width + 7) // 8
    offset = len(blob)
    for frame in frames:
        blob.extend(pack_mask(frame.crop(union)))

    return Animation(
        name=name,
        offset=offset,
        width=width,
        height=height,
        stride=stride,
        frame_count=len(frames),
        source_x=left,
        source_y=top,
        durations=durations,
    )


def append_font(blob: bytearray, name: str, path: Path, size: int) -> Font:
    font = ImageFont.truetype(str(path), size=size)
    ascent, descent = font.getmetrics()
    characters = [chr(code) for code in range(ASCII_FIRST, ASCII_LAST + 1)]
    bboxes = [font.getbbox(character, anchor="la") for character in characters]
    line_height = max(ascent + descent, max(bbox[3] for bbox in bboxes))
    glyphs: list[Glyph] = []

    for character, bbox in zip(characters, bboxes):
        left, _top, right, _bottom = bbox
        advance = max(1, int(round(font.getlength(character))))
        width = max(0, right - left)
        if width == 0 or character == " ":
            glyphs.append(Glyph(0xFFFFFFFF, 0, line_height, left, advance))
            continue

        grayscale = Image.new("L", (width, line_height), 0)
        draw = ImageDraw.Draw(grayscale)
        draw.text((-left, 0), character, font=font, fill=255, anchor="la")
        bitmap = grayscale.point(lambda value: 255 if value >= 112 else 0, mode="1")
        offset = len(blob)
        blob.extend(pack_mask(bitmap))
        glyphs.append(Glyph(offset, width, line_height, left, advance))

    return Font(name=name, line_height=line_height, glyphs=glyphs)


def format_values(values: list[int], indent: str = "    ", per_line: int = 12) -> str:
    lines = []
    for start in range(0, len(values), per_line):
        lines.append(indent + ", ".join(str(value) for value in values[start:start + per_line]) + ",")
    return "\n".join(lines)


def write_header(path: Path, animations: list[Animation], fonts: list[Font], blob_size: int) -> None:
    parts = [
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace pogopo::menu::generated {",
        "",
        "struct AnimationMeta {",
        "    uint32_t offset;",
        "    uint16_t width;",
        "    uint16_t height;",
        "    uint16_t stride;",
        "    uint16_t frame_count;",
        "    int16_t source_x;",
        "    int16_t source_y;",
        "    const uint16_t* durations_ms;",
        "};",
        "",
        "struct GlyphMeta {",
        "    uint32_t offset;",
        "    uint8_t width;",
        "    uint8_t height;",
        "    int8_t x_offset;",
        "    uint8_t advance;",
        "};",
        "",
        "struct FontMeta {",
        "    uint8_t line_height;",
        "    const GlyphMeta* glyphs;",
        "};",
        "",
    ]

    for animation in animations:
        parts.extend([
            f"inline constexpr uint16_t k{animation.name}Durations[] = {{",
            format_values(animation.durations),
            "};",
            "",
        ])

    parts.append("inline constexpr AnimationMeta kAnimations[] = {")
    for animation in animations:
        parts.append(
            f"    {{{animation.offset}U, {animation.width}, {animation.height}, "
            f"{animation.stride}, {animation.frame_count}, {animation.source_x}, "
            f"{animation.source_y}, k{animation.name}Durations}},"
        )
    parts.extend(["};", ""])

    for font in fonts:
        parts.append(f"inline constexpr GlyphMeta k{font.name}Glyphs[] = {{")
        for glyph in font.glyphs:
            offset = "0xFFFFFFFFU" if glyph.offset == 0xFFFFFFFF else f"{glyph.offset}U"
            parts.append(
                f"    {{{offset}, {glyph.width}, {glyph.height}, "
                f"{glyph.x_offset}, {glyph.advance}}},"
            )
        parts.extend(["};", ""])

    parts.append("inline constexpr FontMeta kFonts[] = {")
    for font in fonts:
        parts.append(f"    {{{font.line_height}, k{font.name}Glyphs}},")
    parts.extend([
        "};",
        "",
        f"inline constexpr size_t kBlobSize = {blob_size}U;",
        f"inline constexpr uint8_t kAsciiFirst = {ASCII_FIRST};",
        f"inline constexpr uint8_t kAsciiLast = {ASCII_LAST};",
        "",
        "} // namespace pogopo::menu::generated",
        "",
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(parts), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("docs", type=Path, help="Directory containing the supplied GIFs and OTF files")
    parser.add_argument("blob", type=Path)
    parser.add_argument("header", type=Path)
    args = parser.parse_args()

    blob = bytearray()
    animation_sources = [
        ("Pogopo", "pogopoSHARP.gif"),
        ("GameBoy", "gameboySHARP.gif"),
        ("Playdate", "playdateSHARP.gif"),
        ("GameBoyFull", "gameboychik.gif"),
    ]
    animations = [
        append_animation(blob, name, args.docs / filename)
        for name, filename in animation_sources
    ]

    font_sources = [
        ("Regular14", "pogofont-Regular.otf", 14),
        ("Italic14", "pogofont-RegularItalic.otf", 14),
        ("Regular22", "pogofont-Regular.otf", 22),
        ("Italic22", "pogofont-RegularItalic.otf", 22),
    ]
    fonts = [
        append_font(blob, name, args.docs / filename, size)
        for name, filename, size in font_sources
    ]

    args.blob.parent.mkdir(parents=True, exist_ok=True)
    args.blob.write_bytes(blob)
    write_header(args.header, animations, fonts, len(blob))

    for animation in animations:
        print(
            f"{animation.name}: {animation.frame_count} frames, "
            f"{animation.width}x{animation.height} at "
            f"({animation.source_x},{animation.source_y})"
        )
    for font in fonts:
        print(f"{font.name}: 95 ASCII glyphs, line height {font.line_height}")
    print(f"Packed {len(blob)} bytes -> {args.blob}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
