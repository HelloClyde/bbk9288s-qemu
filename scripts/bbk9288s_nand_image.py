#!/usr/bin/env python3
"""Extract, populate, and repack BBK 9288S raw NAND images."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import os
from pathlib import Path
import shutil
import struct
import sys
from typing import BinaryIO


PAGE_DATA_SIZE = 2048
PAGE_OOB_SIZE = 64
RAW_PAGE_SIZE = PAGE_DATA_SIZE + PAGE_OOB_SIZE
PAGES_PER_BLOCK = 64
SECTORS_PER_PAGE = 4
SECTOR_SIZE = 512
SECTORS_PER_BLOCK = PAGES_PER_BLOCK * SECTORS_PER_PAGE
BLOCK_DATA_SIZE = PAGE_DATA_SIZE * PAGES_PER_BLOCK
RAW_BLOCK_SIZE = RAW_PAGE_SIZE * PAGES_PER_BLOCK
PHYSICAL_BLOCKS = 2048
RESERVED_PHYSICAL_BLOCKS = 40
LOGICAL_SECTORS = 503_808
LOGICAL_BLOCKS = LOGICAL_SECTORS // SECTORS_PER_BLOCK
NAND_RAW_SIZE = PHYSICAL_BLOCKS * RAW_BLOCK_SIZE
FLAT_IMAGE_SIZE = LOGICAL_SECTORS * SECTOR_SIZE
PARTITION_LBA = 32
ERASED_CHUNK = b"\xff" * (1024 * 1024)
ERASED_LOGICAL_BLOCK = b"\xff" * BLOCK_DATA_SIZE


def fill_erased(file: BinaryIO, size: int) -> None:
    remaining = size
    while remaining:
        chunk = ERASED_CHUNK[: min(remaining, len(ERASED_CHUNK))]
        file.write(chunk)
        remaining -= len(chunk)


def check_raw_size(path: Path) -> None:
    size = path.stat().st_size
    if size != NAND_RAW_SIZE:
        raise ValueError(
            f"{path} has {size} bytes; expected {NAND_RAW_SIZE} raw NAND bytes"
        )


def block_tag(file: BinaryIO, physical_block: int) -> int | None:
    tags: Counter[int] = Counter()
    block_offset = physical_block * RAW_BLOCK_SIZE

    for page in range(2):
        file.seek(block_offset + page * RAW_PAGE_SIZE + PAGE_DATA_SIZE)
        oob = file.read(PAGE_OOB_SIZE)
        if len(oob) != PAGE_OOB_SIZE:
            raise ValueError("short read while scanning NAND OOB")
        for sector in range(SECTORS_PER_PAGE):
            slot = sector * 16
            first = struct.unpack_from("<H", oob, slot + 6)[0]
            second = struct.unpack_from("<H", oob, slot + 11)[0]
            if oob[slot + 1] == 0 and first != 0xFFFF and first == second:
                tags[first] += 1

    return tags.most_common(1)[0][0] if tags else None


def scan_mappings(path: Path) -> tuple[dict[int, int], dict[int, list[int]]]:
    check_raw_size(path)
    candidates: dict[int, list[int]] = defaultdict(list)

    with path.open("rb") as file:
        for physical_block in range(RESERVED_PHYSICAL_BLOCKS, PHYSICAL_BLOCKS):
            tag = block_tag(file, physical_block)
            if tag is None:
                continue
            logical_block = tag >> 1
            parity = logical_block.bit_count() & 1
            if logical_block < LOGICAL_BLOCKS and (tag & 1) == parity:
                candidates[logical_block].append(physical_block)

    mappings: dict[int, int] = {}
    duplicates: dict[int, list[int]] = {}
    for logical_block, entries in candidates.items():
        entries.sort()
        mappings[logical_block] = entries[-1]
        if len(entries) > 1:
            duplicates[logical_block] = entries
    return mappings, duplicates


def validate_flat_image(path: Path) -> None:
    size = path.stat().st_size
    if size != FLAT_IMAGE_SIZE:
        raise ValueError(
            f"{path} has {size} bytes; expected {FLAT_IMAGE_SIZE} logical bytes"
        )

    with path.open("rb") as file:
        file.seek(510)
        if file.read(2) != b"\x55\xaa":
            raise ValueError(f"{path} does not contain a valid MBR signature")
        file.seek(PARTITION_LBA * SECTOR_SIZE + 510)
        if file.read(2) != b"\x55\xaa":
            raise ValueError(f"{path} does not contain a valid FAT boot signature")


def extract_image(nand_path: Path, flat_path: Path) -> dict[int, int]:
    mappings, duplicates = scan_mappings(nand_path)
    flat_path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = flat_path.with_name(flat_path.name + ".tmp")

    try:
        with temp_path.open("wb") as flat:
            fill_erased(flat, FLAT_IMAGE_SIZE)
        with nand_path.open("rb") as nand, temp_path.open("r+b") as flat:
            for logical_block, physical_block in sorted(mappings.items()):
                flat.seek(logical_block * BLOCK_DATA_SIZE)
                raw_offset = physical_block * RAW_BLOCK_SIZE
                for page in range(PAGES_PER_BLOCK):
                    nand.seek(raw_offset + page * RAW_PAGE_SIZE)
                    data = nand.read(PAGE_DATA_SIZE)
                    if len(data) != PAGE_DATA_SIZE:
                        raise ValueError("short read while extracting NAND page")
                    flat.write(data)
        os.replace(temp_path, flat_path)
    finally:
        temp_path.unlink(missing_ok=True)

    validate_flat_image(flat_path)
    print(
        f"extracted {len(mappings)} mapped logical blocks to {flat_path} "
        f"({FLAT_IMAGE_SIZE} bytes)"
    )
    if duplicates:
        print(
            f"warning: selected highest physical block for "
            f"{len(duplicates)} maps"
        )
    return mappings


def make_oob(logical_block: int) -> bytes:
    tag = (logical_block << 1) | (logical_block.bit_count() & 1)
    slot = bytearray(b"\xff" * 16)
    slot[1] = 0
    struct.pack_into("<H", slot, 6, tag)
    struct.pack_into("<H", slot, 11, tag)
    return bytes(slot) * SECTORS_PER_PAGE


def pack_image(flat_path: Path, nand_path: Path) -> int:
    validate_flat_image(flat_path)
    nand_path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = nand_path.with_name(nand_path.name + ".tmp")
    used_blocks = 0

    try:
        with temp_path.open("wb") as nand:
            fill_erased(nand, NAND_RAW_SIZE)
        with flat_path.open("rb") as flat, temp_path.open("r+b") as nand:
            for logical_block in range(LOGICAL_BLOCKS):
                data = flat.read(BLOCK_DATA_SIZE)
                if len(data) != BLOCK_DATA_SIZE:
                    raise ValueError("short read while packing logical block")
                if data == ERASED_LOGICAL_BLOCK:
                    continue

                physical_block = RESERVED_PHYSICAL_BLOCKS + used_blocks
                if physical_block >= PHYSICAL_BLOCKS:
                    raise ValueError("logical image needs more physical blocks than NAND provides")
                oob = make_oob(logical_block)
                nand.seek(physical_block * RAW_BLOCK_SIZE)
                for page in range(PAGES_PER_BLOCK):
                    start = page * PAGE_DATA_SIZE
                    nand.write(data[start : start + PAGE_DATA_SIZE])
                    nand.write(oob)
                used_blocks += 1

        os.replace(temp_path, nand_path)
    finally:
        temp_path.unlink(missing_ok=True)

    print(
        f"packed {used_blocks} logical blocks into {nand_path} "
        f"({NAND_RAW_SIZE} raw bytes)"
    )
    return used_blocks


def normalize_fat(flat_path: Path) -> None:
    partition_offset = PARTITION_LBA * SECTOR_SIZE
    with flat_path.open("r+b") as file:
        file.seek(partition_offset)
        bpb = file.read(SECTOR_SIZE)
        if len(bpb) != SECTOR_SIZE:
            raise ValueError("short read while loading FAT boot sector")

        bytes_per_sector = struct.unpack_from("<H", bpb, 11)[0]
        sectors_per_cluster = bpb[13]
        reserved_sectors = struct.unpack_from("<H", bpb, 14)[0]
        number_of_fats = bpb[16]
        root_entries = struct.unpack_from("<H", bpb, 17)[0]
        fat_sectors = struct.unpack_from("<H", bpb, 22)[0]
        if bytes_per_sector != SECTOR_SIZE or number_of_fats != 2:
            raise ValueError("unexpected FAT16 geometry in extracted image")

        fat_offset = partition_offset + reserved_sectors * SECTOR_SIZE
        fat_size = fat_sectors * SECTOR_SIZE
        empty_fat = bytearray(fat_size)
        empty_fat[:4] = b"\xf8\xff\xff\xff"
        for index in range(number_of_fats):
            file.seek(fat_offset + index * fat_size)
            file.write(empty_fat)

        root_offset = fat_offset + number_of_fats * fat_size
        root_size = root_entries * 32
        file.seek(root_offset)
        file.write(b"\x00" * root_size)

        data_offset = root_offset + root_size
        cluster_size = sectors_per_cluster * SECTOR_SIZE
        file.seek(data_offset)
        file.write(b"\xff" * (cluster_size * 16))

    print(
        f"normalized FAT16: cluster={cluster_size} reserved={reserved_sectors} "
        f"fat_sectors={fat_sectors} root_entries={root_entries}"
    )


def fat_short_name_checksum(name: bytes) -> int:
    checksum = 0
    for value in name:
        checksum = ((checksum & 1) << 7) + (checksum >> 1) + value
        checksum &= 0xFF
    return checksum


def gbk_short_name(name: str) -> bytes | None:
    if name.isascii():
        return None

    if "." in name and not name.startswith("."):
        base, extension = name.rsplit(".", 1)
    else:
        base, extension = name, ""

    try:
        base_bytes = base.upper().encode("gbk")
        extension_bytes = extension.upper().encode("gbk")
    except UnicodeEncodeError:
        return None
    if not base_bytes or len(base_bytes) > 8 or len(extension_bytes) > 3:
        return None

    return base_bytes.ljust(8, b" ") + extension_bytes.ljust(3, b" ")


def decode_lfn_fragment(entry: bytes) -> str:
    encoded = entry[1:11] + entry[14:26] + entry[28:32]
    chars = []
    for offset in range(0, len(encoded), 2):
        value = struct.unpack_from("<H", encoded, offset)[0]
        if value in (0x0000, 0xFFFF):
            continue
        chars.append(chr(value))
    return "".join(chars)


def patch_gbk_short_names(flat_path: Path) -> int:
    """Make VFAT entries visible to the firmware's GBK 8.3 path lookup."""
    partition_offset = PARTITION_LBA * SECTOR_SIZE
    patched = 0

    with flat_path.open("r+b") as file:
        file.seek(partition_offset)
        bpb = file.read(SECTOR_SIZE)
        bytes_per_sector = struct.unpack_from("<H", bpb, 11)[0]
        sectors_per_cluster = bpb[13]
        reserved_sectors = struct.unpack_from("<H", bpb, 14)[0]
        number_of_fats = bpb[16]
        root_entries = struct.unpack_from("<H", bpb, 17)[0]
        fat_sectors = struct.unpack_from("<H", bpb, 22)[0]

        fat_offset = partition_offset + reserved_sectors * bytes_per_sector
        root_offset = fat_offset + number_of_fats * fat_sectors * bytes_per_sector
        root_size = root_entries * 32
        data_offset = root_offset + root_size
        cluster_size = sectors_per_cluster * bytes_per_sector

        file.seek(fat_offset)
        fat = file.read(fat_sectors * bytes_per_sector)

        def cluster_chain(first_cluster: int) -> list[int]:
            chain = []
            seen = set()
            cluster = first_cluster
            while 2 <= cluster < 0xFFF8 and cluster not in seen:
                seen.add(cluster)
                chain.append(cluster)
                cluster = struct.unpack_from("<H", fat, cluster * 2)[0]
            return chain

        def directory_offsets(first_cluster: int | None) -> list[int]:
            if first_cluster is None:
                return list(range(root_offset, root_offset + root_size, 32))
            offsets = []
            for cluster in cluster_chain(first_cluster):
                start = data_offset + (cluster - 2) * cluster_size
                offsets.extend(range(start, start + cluster_size, 32))
            return offsets

        pending: list[int | None] = [None]
        visited = set()
        while pending:
            directory = pending.pop()
            if directory is not None:
                if directory in visited:
                    continue
                visited.add(directory)

            lfn_entries: list[tuple[int, bytes]] = []
            for entry_offset in directory_offsets(directory):
                file.seek(entry_offset)
                entry = file.read(32)
                if len(entry) != 32 or entry[0] == 0x00:
                    break
                if entry[0] == 0xE5:
                    lfn_entries.clear()
                    continue
                if entry[11] == 0x0F:
                    lfn_entries.append((entry_offset, entry))
                    continue

                is_directory = bool(entry[11] & 0x10)
                first_cluster = struct.unpack_from("<H", entry, 26)[0]
                if is_directory and entry[0] != ord(".") and first_cluster >= 2:
                    pending.append(first_cluster)

                if lfn_entries:
                    long_name = "".join(
                        decode_lfn_fragment(item[1])
                        for item in sorted(
                            lfn_entries, key=lambda item: item[1][0] & 0x1F
                        )
                    )
                    replacement = gbk_short_name(long_name)
                    if replacement is not None and replacement != entry[:11]:
                        checksum = fat_short_name_checksum(replacement)
                        file.seek(entry_offset)
                        file.write(replacement)
                        for lfn_offset, lfn_entry in lfn_entries:
                            updated = bytearray(lfn_entry)
                            updated[13] = checksum
                            file.seek(lfn_offset)
                            file.write(updated)
                        patched += 1
                lfn_entries.clear()

    print(f"patched {patched} FAT short names for GBK firmware lookup")
    return patched


def populate_fat(
    flat_path: Path,
    source_path: Path,
    target_name: str,
    encoding: str,
) -> tuple[int, int]:
    try:
        from pyfatfs.PyFatFS import PyFatFS
    except ImportError as exc:
        raise RuntimeError("pyfatfs is required for the install command") from exc

    if not source_path.is_dir():
        raise ValueError(f"system source is not a directory: {source_path}")
    validate_flat_image(flat_path)
    normalize_fat(flat_path)

    target_root = "/" + target_name.strip("/")
    files = 0
    total_bytes = 0
    fat = PyFatFS(
        str(flat_path),
        encoding=encoding,
        offset=PARTITION_LBA * SECTOR_SIZE,
        preserve_case=True,
        read_only=False,
    )
    try:
        fat.makedirs(target_root, recreate=True)
        paths = sorted(
            source_path.rglob("*"),
            key=lambda path: (not path.is_dir(), str(path)),
        )
        for source in paths:
            relative = source.relative_to(source_path).as_posix()
            target = f"{target_root}/{relative}"
            if source.is_dir():
                fat.makedirs(target, recreate=True)
                continue
            if not source.is_file():
                continue
            with source.open("rb") as src, fat.openbin(target, "w") as dst:
                shutil.copyfileobj(src, dst, length=1024 * 1024)
            files += 1
            total_bytes += source.stat().st_size
            if files % 25 == 0:
                print(f"installed {files} files ({total_bytes} bytes)")
    finally:
        fat.close()

    patch_gbk_short_names(flat_path)

    verify_path = f"{target_root}/\u6570\u636e/HZK_LIB.BIN"
    verify = PyFatFS(
        str(flat_path),
        encoding=encoding,
        offset=PARTITION_LBA * SECTOR_SIZE,
        read_only=True,
    )
    try:
        info = verify.getinfo(verify_path, namespaces=["details"])
        hzk_size = info.size
    finally:
        verify.close()
    print(
        f"installed {files} files ({total_bytes} bytes); "
        f"verified {verify_path} ({hzk_size} bytes)"
    )
    return files, total_bytes


def command_info(args: argparse.Namespace) -> None:
    mappings, duplicates = scan_mappings(args.nand)
    print(f"raw_size={NAND_RAW_SIZE}")
    print(f"mapped_blocks={len(mappings)}")
    print(f"duplicate_maps={len(duplicates)}")
    for logical_block, physical_block in sorted(mappings.items()):
        print(f"logical={logical_block} physical={physical_block}")


def command_extract(args: argparse.Namespace) -> None:
    extract_image(args.nand, args.flat)


def command_pack(args: argparse.Namespace) -> None:
    pack_image(args.flat, args.nand)


def command_install(args: argparse.Namespace) -> None:
    flat_path = args.flat or args.nand.with_name(args.nand.stem + ".fat.img")
    extract_image(args.nand, flat_path)
    populate_fat(flat_path, args.source, args.target, args.encoding)
    pack_image(flat_path, args.output or args.nand)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    info = subparsers.add_parser("info", help="show FTL block mappings")
    info.add_argument("nand", type=Path)
    info.set_defaults(func=command_info)

    extract = subparsers.add_parser("extract", help="extract a flat FAT disk image")
    extract.add_argument("nand", type=Path)
    extract.add_argument("flat", type=Path)
    extract.set_defaults(func=command_extract)

    pack = subparsers.add_parser("pack", help="pack a flat disk into raw NAND")
    pack.add_argument("flat", type=Path)
    pack.add_argument("nand", type=Path)
    pack.set_defaults(func=command_pack)

    install = subparsers.add_parser("install", help="install a system tree into NAND")
    install.add_argument("nand", type=Path, help="formatted source NAND image")
    install.add_argument("source", type=Path, help="host system directory")
    install.add_argument("--output", type=Path, help="output NAND; default replaces input")
    install.add_argument("--flat", type=Path, help="keep the intermediate flat image here")
    install.add_argument("--target", default="\u7cfb\u7edf", help="target FAT directory")
    install.add_argument(
        "--encoding", default="ibm437", help="FAT short-name encoding"
    )
    install.set_defaults(func=command_install)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        args.func(args)
    except (OSError, ValueError, RuntimeError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
