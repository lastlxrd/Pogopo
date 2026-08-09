#!/usr/bin/env python3
"""Pack numbered SharpBit C assets into one display-native animation blob."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


BYTE_RE = re.compile(r"0x([0-9A-Fa-f]{2})")


def reverse_bits(value: int) -> int:
    value = ((value & 0x55) << 1) | ((value >> 1) & 0x55)
    value = ((value & 0x33) << 2) | ((value >> 2) & 0x33)
    return ((value & 0x0F) << 4) | ((value >> 4) & 0x0F)


def load_asset(path: Path, expected_size: int) -> bytes:
    text = path.read_text(encoding="utf-8")
    array_start = text.find("{")
    array_end = text.find("};", array_start)
    if array_start < 0 or array_end < 0:
        raise RuntimeError(f"Cannot find byte array in {path}")
    data = bytes(int(match, 16) for match in BYTE_RE.findall(text[array_start:array_end]))
    if len(data) != expected_size:
        raise RuntimeError(
            f"{path} contains {len(data)} bytes; expected {expected_size}"
        )
    return data


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("generated_dir", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--prefix", default="pogopo_scenee")
    parser.add_argument("--frames", type=int, default=122)
    parser.add_argument("--width", type=int, default=400)
    parser.add_argument("--height", type=int, default=240)
    args = parser.parse_args()

    if args.width <= 0 or args.height <= 0 or args.width % 8:
        raise RuntimeError("Width must be positive and divisible by 8")
    if args.frames <= 0:
        raise RuntimeError("Frame count must be positive")

    frame_size = (args.width // 8) * args.height
    packed = bytearray()
    for number in range(1, args.frames + 1):
        symbol = f"{args.prefix}{number}"
        source = args.generated_dir / symbol / f"{symbol}.c"
        raw = load_asset(source, frame_size)

        # SharpBit's repository defaults are MSB-first with black=1. Pogopo's
        # Sharp framebuffer is LSB-first with white=1, so convert once here and
        # let the firmware copy every frame directly to the LCD framebuffer.
        packed.extend(reverse_bits(value ^ 0xFF) for value in raw)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(packed)
    print(
        f"Packed {args.frames} frames, {frame_size} bytes each, "
        f"{len(packed)} bytes total -> {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
