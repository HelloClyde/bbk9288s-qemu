#!/usr/bin/env python3
"""Find little-endian 32-bit values in BBK 9288S KNL images."""

from __future__ import annotations

import argparse
from pathlib import Path


KERNEL_HEADER_SIZE = 0x40
DEFAULT_LOAD_BASE = 0x02000000


def parse_u32(value: str) -> int:
    return int(value, 0) & 0xFFFFFFFF


def load_image(path: Path) -> bytes:
    raw = path.read_bytes()
    if raw[:4] == b"KNL ":
        return raw[KERNEL_HEADER_SIZE:]
    return raw


def hex_bytes(data: bytes) -> str:
    return " ".join(f"{byte:02x}" for byte in data)


def ascii_bytes(data: bytes) -> str:
    chars = []
    for byte in data:
        chars.append(chr(byte) if 0x20 <= byte <= 0x7E else ".")
    return "".join(chars)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", default="build/bbk9288s-test/kernel.bin")
    parser.add_argument("--load-base", type=parse_u32, default=DEFAULT_LOAD_BASE)
    parser.add_argument("--value", type=parse_u32, action="append", required=True)
    parser.add_argument("--align", type=int, default=1)
    parser.add_argument("--context", type=int, default=16)
    parser.add_argument("--limit", type=int, default=200)
    args = parser.parse_args()

    if args.align <= 0:
        raise SystemExit("--align must be positive")

    data = load_image(Path(args.kernel))
    needles = {value: value.to_bytes(4, "little") for value in args.value}
    total = 0

    for value, needle in needles.items():
        matches: list[int] = []
        start = 0
        while True:
            offset = data.find(needle, start)
            if offset < 0:
                break
            if offset % args.align == 0:
                matches.append(offset)
            start = offset + 1

        print(f"0x{value:08x}: matches={len(matches)}")
        total += len(matches)
        for offset in matches[: args.limit]:
            left = max(0, offset - args.context)
            right = min(len(data), offset + 4 + args.context)
            chunk = data[left:right]
            addr = args.load_base + offset
            marker = offset - left
            print(f"  addr=0x{addr:08x} offset=0x{offset:06x}")
            print(f"    bytes: {hex_bytes(chunk)}")
            print(f"    ascii: {ascii_bytes(chunk)}")
            print(f"    mark:  {'   ' * marker}^^ ^^ ^^ ^^")
        if len(matches) > args.limit:
            print(f"  ... {len(matches) - args.limit} more")

    print(f"total={total}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
