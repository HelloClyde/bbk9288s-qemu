#!/usr/bin/env python3
"""Scan BBK 9288S KNL images for S1C33 relative control-flow references."""

from __future__ import annotations

import argparse
from pathlib import Path


KERNEL_HEADER_SIZE = 0x40
DEFAULT_LOAD_BASE = 0x02000000


def parse_u32(value: str) -> int:
    return int(value, 0)


def sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def branch_disp(word: int, ext: list[int]) -> int:
    sign8 = word & 0xff
    if len(ext) == 0:
        return sign_extend(sign8, 8) * 2
    if len(ext) == 1:
        disp = (ext[0] << 9) | (sign8 << 1)
        return sign_extend(disp, 22)
    disp = ((ext[0] >> 3) << 22) | (ext[1] << 9) | (sign8 << 1)
    return sign_extend(disp, 32)


def rel_kind(word: int) -> str | None:
    if 0x1c00 <= word <= 0x1dff:
        return "call"
    if 0x1e00 <= word <= 0x1fff:
        return "jp"
    if 0x0800 <= word <= 0x1bff:
        op1 = (word >> 9) & 0x0f
        if 4 <= op1 <= 13:
            return "branch"
    return None


def in_range(value: int, start: int | None, end: int | None) -> bool:
    if start is not None and value < start:
        return False
    if end is not None and value > end:
        return False
    return True


def scan(data: bytes, base: int, args: argparse.Namespace) -> list[tuple[str, int, int, int]]:
    refs: list[tuple[str, int, int, int]] = []
    ext: list[int] = []

    for offset in range(0, len(data) - 1, 2):
        pc = base + offset
        word = data[offset] | (data[offset + 1] << 8)

        if (word & 0xe000) == 0xc000:
            imm13 = word & 0x1fff
            if len(ext) == 1:
                ext = [ext[0], imm13]
            else:
                ext = [imm13]
            continue

        kind = rel_kind(word)
        if kind is not None and (args.kind == "all" or args.kind == kind):
            target = (pc + branch_disp(word, ext)) & 0xffffffff
            if in_range(target, args.target_start, args.target_end) and in_range(
                pc, args.from_start, args.from_end
            ):
                refs.append((kind, pc, target, word))
        ext = []

    return refs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", default="build/bbk9288s-test/kernel.bin")
    parser.add_argument("--load-base", type=parse_u32, default=DEFAULT_LOAD_BASE)
    parser.add_argument("--target-start", type=parse_u32)
    parser.add_argument("--target-end", type=parse_u32)
    parser.add_argument("--from-start", type=parse_u32)
    parser.add_argument("--from-end", type=parse_u32)
    parser.add_argument("--kind", choices=["all", "call", "jp", "branch"], default="call")
    parser.add_argument("--limit", type=int, default=200)
    args = parser.parse_args()

    path = Path(args.kernel)
    raw = path.read_bytes()
    if raw[:4] == b"KNL ":
        raw = raw[KERNEL_HEADER_SIZE:]

    refs = scan(raw, args.load_base, args)
    print(f"refs: {len(refs)}")
    for kind, pc, target, word in refs[: args.limit]:
        print(f"{kind:6s} from=0x{pc:08x} to=0x{target:08x} word=0x{word:04x}")
    if len(refs) > args.limit:
        print(f"... {len(refs) - args.limit} more")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
