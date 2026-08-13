#!/usr/bin/env python3
"""Pack an Aseprite animation as Sharp framebuffer color+opacity planes.

Each output frame contains a 12 KiB native 1-bit color plane followed by a
12 KiB native 1-bit opacity mask. Transparent source pixels have a zero mask
bit, so firmware can preserve the menu/game framebuffer below the animation.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path


ASEPRITE_MAGIC = 0xA5E0
FRAME_MAGIC = 0xF1FA
LAYER_CHUNK = 0x2004
CEL_CHUNK = 0x2005


@dataclass(frozen=True)
class Layer:
    flags: int
    layer_type: int
    blend_mode: int
    opacity: int


@dataclass(frozen=True)
class Cel:
    x: int
    y: int
    opacity: int
    width: int
    height: int
    pixels: bytes


def read_aseprite(path: Path) -> tuple[int, int, list[Layer], list[dict[int, Cel]]]:
    data = path.read_bytes()
    if len(data) < 128:
        raise RuntimeError("Aseprite file is shorter than its 128-byte header")

    file_size, magic, frame_count, width, height, depth = struct.unpack_from(
        "<IHHHHH", data, 0
    )
    if file_size != len(data) or magic != ASEPRITE_MAGIC:
        raise RuntimeError("Invalid Aseprite file header")
    if depth != 16:
        raise RuntimeError(
            f"Aseprite color depth is {depth}; expected 16-bit grayscale+alpha"
        )

    layers: list[Layer] = []
    frames: list[dict[int, Cel]] = []
    offset = 128
    for frame_index in range(frame_count):
        if offset + 16 > len(data):
            raise RuntimeError(f"Frame {frame_index + 1} header is truncated")
        frame_size, frame_magic, old_count, _duration, _reserved, new_count = (
            struct.unpack_from("<IHHHHI", data, offset)
        )
        if frame_magic != FRAME_MAGIC or frame_size < 16:
            raise RuntimeError(f"Frame {frame_index + 1} has an invalid header")
        frame_end = offset + frame_size
        if frame_end > len(data):
            raise RuntimeError(f"Frame {frame_index + 1} is truncated")

        chunk_count = new_count or old_count
        chunk_offset = offset + 16
        frame_cels: dict[int, Cel] = {}
        for _ in range(chunk_count):
            if chunk_offset + 6 > frame_end:
                raise RuntimeError(f"Frame {frame_index + 1} chunk is truncated")
            chunk_size, chunk_type = struct.unpack_from("<IH", data, chunk_offset)
            chunk_end = chunk_offset + chunk_size
            if chunk_size < 6 or chunk_end > frame_end:
                raise RuntimeError(f"Frame {frame_index + 1} has a bad chunk size")

            if chunk_type == LAYER_CHUNK:
                flags, layer_type, _level, _w, _h, blend_mode = struct.unpack_from(
                    "<HHHHHH", data, chunk_offset + 6
                )
                opacity = data[chunk_offset + 18]
                layers.append(Layer(flags, layer_type, blend_mode, opacity))

            elif chunk_type == CEL_CHUNK:
                layer_index, x, y = struct.unpack_from("<Hhh", data, chunk_offset + 6)
                opacity = data[chunk_offset + 12]
                cel_type = struct.unpack_from("<H", data, chunk_offset + 13)[0]
                if cel_type in (0, 2):
                    cel_width, cel_height = struct.unpack_from(
                        "<HH", data, chunk_offset + 22
                    )
                    pixels = data[chunk_offset + 26 : chunk_end]
                    if cel_type == 2:
                        pixels = zlib.decompress(pixels)
                    expected = cel_width * cel_height * 2
                    if len(pixels) != expected:
                        raise RuntimeError(
                            f"Frame {frame_index + 1}, layer {layer_index}: "
                            f"decoded {len(pixels)} bytes; expected {expected}"
                        )
                    frame_cels[layer_index] = Cel(
                        x, y, opacity, cel_width, cel_height, pixels
                    )
                elif cel_type == 1:
                    linked_frame = struct.unpack_from("<H", data, chunk_offset + 22)[0]
                    if linked_frame >= len(frames) or layer_index not in frames[linked_frame]:
                        raise RuntimeError(
                            f"Frame {frame_index + 1}, layer {layer_index}: "
                            f"invalid link to frame {linked_frame + 1}"
                        )
                    linked = frames[linked_frame][layer_index]
                    frame_cels[layer_index] = Cel(
                        x,
                        y,
                        opacity,
                        linked.width,
                        linked.height,
                        linked.pixels,
                    )
                else:
                    raise RuntimeError(
                        f"Frame {frame_index + 1}, layer {layer_index}: "
                        f"unsupported cel type {cel_type}"
                    )

            chunk_offset = chunk_end

        if chunk_offset != frame_end:
            raise RuntimeError(f"Frame {frame_index + 1} chunk table is misaligned")
        frames.append(frame_cels)
        offset = frame_end

    if offset != len(data):
        raise RuntimeError("Trailing bytes after the last Aseprite frame")
    return width, height, layers, frames


def composite_frame(
    width: int, height: int, layers: list[Layer], cels: dict[int, Cel]
) -> tuple[bytearray, bytearray]:
    # Aseprite stores layers from bottom to top. This supplied animation uses
    # normal blending and hard 0/255 grayscale/alpha pixels, so compositing is
    # exact and requires no lossy thresholding.
    colors = bytearray(width * height)
    alphas = bytearray(width * height)
    for layer_index, layer in enumerate(layers):
        if not (layer.flags & 1) or layer.layer_type != 0:
            continue
        if layer.blend_mode != 0:
            raise RuntimeError(
                f"Layer {layer_index} uses unsupported blend mode {layer.blend_mode}"
            )
        cel = cels.get(layer_index)
        if cel is None:
            continue
        if layer.opacity != 255 or cel.opacity != 255:
            raise RuntimeError(
                f"Layer {layer_index} uses partial layer/cel opacity"
            )

        for source_y in range(cel.height):
            target_y = cel.y + source_y
            if target_y < 0 or target_y >= height:
                continue
            source_row = source_y * cel.width * 2
            target_row = target_y * width
            for source_x in range(cel.width):
                target_x = cel.x + source_x
                if target_x < 0 or target_x >= width:
                    continue
                source = source_row + source_x * 2
                gray = cel.pixels[source]
                alpha = cel.pixels[source + 1]
                if gray not in (0, 255) or alpha not in (0, 255):
                    raise RuntimeError(
                        f"Layer {layer_index} has a non-1-bit pixel at "
                        f"({target_x},{target_y}): gray={gray}, alpha={alpha}"
                    )
                if alpha:
                    target = target_row + target_x
                    colors[target] = gray
                    alphas[target] = 255
    return colors, alphas


def pack_plane(pixels: bytearray, width: int, height: int) -> bytes:
    stride = width // 8
    packed = bytearray(stride * height)
    for y in range(height):
        source_row = y * width
        target_row = y * stride
        for x in range(width):
            if pixels[source_row + x]:
                packed[target_row + (x >> 3)] |= 1 << (x & 7)
    return bytes(packed)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--frames", type=int, default=25)
    parser.add_argument("--width", type=int, default=400)
    parser.add_argument("--height", type=int, default=240)
    args = parser.parse_args()

    if args.width <= 0 or args.height <= 0 or args.width % 8:
        raise RuntimeError("Width must be positive and divisible by 8")

    width, height, layers, frames = read_aseprite(args.source)
    if (width, height) != (args.width, args.height):
        raise RuntimeError(
            f"Animation is {width}x{height}; expected {args.width}x{args.height}"
        )
    if len(frames) != args.frames:
        raise RuntimeError(
            f"Animation has {len(frames)} frames; expected {args.frames}"
        )

    packed = bytearray()
    opaque_counts: list[int] = []
    for cels in frames:
        colors, alphas = composite_frame(width, height, layers, cels)
        packed.extend(pack_plane(colors, width, height))
        packed.extend(pack_plane(alphas, width, height))
        opaque_counts.append(sum(1 for alpha in alphas if alpha))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(packed)
    print(
        f"Packed {len(frames)} Aseprite overlay frames, "
        f"{width // 8 * height * 2} bytes each, {len(packed)} bytes total"
    )
    print("Opaque pixels per frame:", ", ".join(map(str, opaque_counts)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
