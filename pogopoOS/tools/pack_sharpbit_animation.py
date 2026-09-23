#!/usr/bin/env python3
"""Pack one monolithic SharpBit C animation into a display-native blob."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


BYTE_RE = re.compile(r"0x([0-9A-Fa-f]{2})")


def reverse_bits(value: int) -> int:
    value = ((value & 0x55) << 1) | ((value >> 1) & 0x55)
    value = ((value & 0x33) << 2) | ((value >> 2) & 0x33)
    return ((value & 0x0F) << 4) | ((value >> 4) & 0x0F)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--symbol", default="sharpbit_animation_pogopointro")
    parser.add_argument("--frames", type=int, default=25)
    parser.add_argument("--width", type=int, default=400)
    parser.add_argument("--height", type=int, default=240)
    args = parser.parse_args()

    if args.width <= 0 or args.height <= 0 or args.width % 8:
        raise RuntimeError("Width must be positive and divisible by 8")
    if args.frames <= 0:
        raise RuntimeError("Frame count must be positive")

    text = args.source.read_text(encoding="utf-8")
    pattern = re.compile(
        rf"static\s+const\s+uint8_t\s+"
        rf"{re.escape(args.symbol)}_frame_(\d+)\[\][^{{]*\{{(.*?)\}};",
        re.DOTALL,
    )
    arrays = {
        int(index): bytes(int(value, 16) for value in BYTE_RE.findall(body))
        for index, body in pattern.findall(text)
    }

    expected_indices = set(range(args.frames))
    if set(arrays) != expected_indices:
        missing = sorted(expected_indices - set(arrays))
        extra = sorted(set(arrays) - expected_indices)
        raise RuntimeError(f"Frame index mismatch: missing={missing}, extra={extra}")

    frame_size = (args.width // 8) * args.height
    packed = bytearray()
    for index in range(args.frames):
        raw = arrays[index]
        if len(raw) != frame_size:
            raise RuntimeError(
                f"Frame {index + 1} contains {len(raw)} bytes; expected {frame_size}"
            )

        # SharpBit exports MSB-first with black=1. Pogopo's Sharp framebuffer
        # is LSB-first with white=1, so convert each pixel once at build time.
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
