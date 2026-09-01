#!/usr/bin/env python3
"""Pack a 1-bit 400x240 GIF into Pogopo's native Sharp framebuffer format."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


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

    stride = args.width // 8
    packed = bytearray()
    with Image.open(args.source) as animation:
        if animation.size != (args.width, args.height):
            raise RuntimeError(
                f"GIF is {animation.width}x{animation.height}; "
                f"expected {args.width}x{args.height}"
            )
        if animation.n_frames != args.frames:
            raise RuntimeError(
                f"GIF has {animation.n_frames} frames; expected {args.frames}"
            )

        for frame_index in range(animation.n_frames):
            # Pillow composites GIF subframes/disposal onto the logical canvas
            # when seeking sequentially, which is the image the user previewed.
            animation.seek(frame_index)
            frame = animation.convert("RGB")
            pixels = frame.load()
            native = bytearray(stride * args.height)
            for y in range(args.height):
                for x in range(args.width):
                    red, green, blue = pixels[x, y]
                    if (red, green, blue) not in ((0, 0, 0), (255, 255, 255)):
                        raise RuntimeError(
                            f"Frame {frame_index + 1} has a non-1-bit pixel "
                            f"at ({x},{y}): {(red, green, blue)}"
                        )
                    # Sharp framebuffer: LSB is the left-most pixel, 1=white.
                    if red == 255:
                        native[y * stride + (x >> 3)] |= 1 << (x & 7)
            packed.extend(native)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(packed)
    print(
        f"Packed {args.frames} GIF frames, {stride * args.height} bytes each, "
        f"{len(packed)} bytes total -> {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
