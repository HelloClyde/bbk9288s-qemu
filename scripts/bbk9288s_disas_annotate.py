#!/usr/bin/env python3
"""Annotate S1C33 instructions in BBK 9288S KNL images."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from pathlib import Path


KERNEL_HEADER_SIZE = 0x40
DEFAULT_LOAD_BASE = 0x02000000


LD_NAMES = ("ld.b", "ld.ub", "ld.h", "ld.uh", "ld.w", "ld.b", "ld.h", "ld.w")
REG_OP_NAMES = ("add", "sub", "cmp", "ld.w", "and", "or", "xor", "not")
BRANCH_NAMES = {
    4: "jrgt",
    5: "jrge",
    6: "jrlt",
    7: "jrle",
    8: "jrugt",
    9: "jruge",
    10: "jrult",
    11: "jrule",
    12: "jreq",
    13: "jrne",
}
BITOP_NAMES = {2: "btst", 3: "bclr", 4: "bset", 5: "bnot"}
TRANSFER_NAMES = ("ld.b", "ld.ub", "ld.h", "ld.uh")
SHIFT_NAMES = {2: "srl", 3: "sll", 4: "sra", 5: "sla", 6: "rr", 7: "rl"}
SCAN_NAMES = {2: "scan0", 3: "scan1"}
DIV_NAMES = {2: "div0s", 3: "div0u", 4: "div1", 5: "div2s", 6: "div3s"}
MUL_NAMES = ("mlt.h", "mltu.h", "mlt.w", "mltu.w")
SREG_NAMES = ("psr", "sp", "alr", "ahr")


KNOWN_ADDRS = {
    0x00003FC0: "gAlarmInfo",
    0x00003FC8: "wake_up_flag",
    0x00003FC9: "gAlarmComing",
    0x00003FCA: "gAmrResidentFlag",
    0x00003FCC: "gCalibratePoint.X",
    0x00003FCE: "gCalibratePoint.Y",
    0x00003FD0: "gfStep_X",
    0x00003FD4: "gfStep_Y",
    0x00003FD8: "g_ZWImeSave",
    0x00003FDC: "gPowerOffState",
    0x00003FDD: "cTimeFlag",
    0x00003FFB: "low event/touch byte 3ffb",
    0x00040150: "CTM",
    0x00040240: "ADD",
    0x00040242: "ADTRG",
    0x00040243: "ADCH",
    0x00040244: "ADCTL",
    0x00040245: "ADSMP",
    0x00040268: "ITC/EAD",
    0x00040275: "8TM enable/factor",
    0x00040277: "ITC/F8TM",
    0x00040280: "port input 4 factor",
    0x00040285: "8TM enable/factor",
    0x00040287: "ITC/FCTM/F8TM",
    0x000402C0: "CFK5",
    0x000402C3: "CFK6",
    0x00300020: "touch serial clock/data",
    0x00300049: "touch board config",
    0x022681F4: "key/touch debounce mode",
    0x022681F5: "key/touch first scan",
    0x022681F6: "key/touch debounce active",
    0x022681FA: "key/touch latest scan",
    0x022681FB: "long-press/event gate",
    0x022F8554: "pen status/value cache",
    0x022F8560: "pen/event flag 8560",
    0x022F8568: "pen helper value",
    0x022F8569: "pen x cache",
    0x022F856A: "pen y cache",
    0x022F8574: "pen/event pointer 8574",
    0x022F8578: "pen/event code source",
    0x022F8590: "pen/event state 8590",
    0x022F85F4: "pen/event state 85f4",
    0x022F85FC: "pen/event state 85fc",
    0x022F8600: "pen/event state 8600",
    0x02292360: "battery/power state",
    0x02380002: "touch gate 02",
    0x02380003: "touch x latched",
    0x02380004: "touch gate 04",
    0x02380005: "touch y latched",
    0x02380006: "touch gate 06",
    0x02380007: "touch status latched",
    0x02380008: "touch gate 08",
    0x02380009: "touch gate 09",
    0x023800B0: "debounce state",
    0x023806D4: "screen width 160",
    0x023806D8: "screen height 240",
    0x0238F138: "event gate 8f138",
    0x0238F148: "pen setter source byte",
    0x0238F14C: "pen setter/event byte 8f14c",
    0x0238F15C: "pen setter/event byte 8f15c",
    0x0238F160: "pen setter/event byte 8f160",
    0x0238F161: "pen setter/event byte 8f161",
    0x0238F404: "event/touch mode 8f404",
    0x0238F405: "event/touch mode 8f405",
}


@dataclass
class Ref:
    addr: int
    access: str
    size: int


@dataclass
class Decoded:
    pc: int
    words: list[int]
    text: str
    comments: list[str] = field(default_factory=list)
    refs: list[Ref] = field(default_factory=list)
    immediates: list[int] = field(default_factory=list)
    call_targets: list[int] = field(default_factory=list)


def parse_u32(value: str) -> int:
    return int(value, 0)


def sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def u32(value: int) -> int:
    return value & 0xFFFFFFFF


def hword(data: bytes, offset: int) -> int:
    return data[offset] | (data[offset + 1] << 8)


def reg(num: int) -> str:
    return f"%r{num}"


def sreg(num: int) -> str:
    return f"%{SREG_NAMES[num] if num < len(SREG_NAMES) else 'reserved'}"


def branch_disp(word: int, ext: list[int]) -> int:
    sign8 = word & 0xFF
    if not ext:
        return sign_extend(sign8, 8) * 2
    if len(ext) == 1:
        disp = (ext[0] << 9) | (sign8 << 1)
        return sign_extend(disp, 22)
    disp = ((ext[0] >> 3) << 22) | (ext[1] << 9) | (sign8 << 1)
    return sign_extend(disp, 32)


def ext_addr(ext: list[int]) -> int | None:
    if not ext:
        return None
    if len(ext) == 1:
        return ext[0]
    return ((ext[0] << 13) | ext[1]) & 0xFFFFFFFF


def ext_imm6(word: int, ext: list[int], signed: bool) -> int:
    imm6 = (word >> 4) & 0x3F
    if not ext:
        return sign_extend(imm6, 6) if signed else imm6
    if len(ext) == 1:
        value = (ext[0] << 6) | imm6
        return sign_extend(value, 19) if signed else value
    return u32((ext[0] << 19) | (ext[1] << 6) | imm6)


def ext_imm13_26(ext: list[int]) -> int | None:
    if not ext:
        return None
    if len(ext) == 1:
        return ext[0]
    return ((ext[0] << 13) | ext[1]) & 0xFFFFFFFF


def add_addr_comment(
    decoded: Decoded,
    rb: int,
    ext: list[int],
    access: str,
    size: int,
    reg_consts: dict[int, int],
) -> None:
    base = ext_addr(ext)
    if base is None:
        const_base = reg_consts.get(rb)
        if const_base is not None:
            decoded.comments.append(label_addr(const_base, f"{access}{size} abs=0x{const_base:08x}"))
            decoded.refs.append(Ref(const_base, access, size))
        return
    if len(ext) == 1:
        decoded.comments.append(f"{access}{size} addr={reg(rb)}+0x{base:x}")
        return
    if rb == 15:
        decoded.comments.append(label_addr(base, f"{access}{size} abs=0x{base:08x}"))
    else:
        decoded.comments.append(f"{access}{size} addr={reg(rb)}+0x{base:08x}")
    decoded.refs.append(Ref(base, access, size))


def add_sp_comment(decoded: Decoded, word: int, ext: list[int], size: int, access: str) -> None:
    imm6 = (word >> 4) & 0x3F
    if not ext:
        offset = imm6 * size
    elif len(ext) == 1:
        offset = ((ext[0] << 6) | imm6) * size
    else:
        offset = ((ext[0] << 19) | (ext[1] << 6) | imm6) * size
    decoded.comments.append(f"{access}{size} sp+0x{offset:x}")


def label_addr(addr: int, prefix: str) -> str:
    name = KNOWN_ADDRS.get(addr)
    if name is not None:
        return f"{prefix} {name}"
    return prefix


def decode_normal(
    pc: int,
    words: list[int],
    ext: list[int],
    reg_consts: dict[int, int],
) -> Decoded:
    word = words[-1]
    decoded = Decoded(pc=pc, words=words, text=f".hword 0x{word:04x}")

    if word == 0x0000:
        decoded.text = "nop"
        return decoded
    if word == 0x0400:
        decoded.text = "brk"
        return decoded
    if word == 0x0040:
        decoded.text = "slp"
        return decoded
    if word == 0x0080:
        decoded.text = "halt"
        return decoded
    if 0xBF40 <= word <= 0xBF5F:
        decoded.text = f"psrset 0x{word & 0x1f:x}"
        return decoded
    if 0xBF60 <= word <= 0xBF7F:
        decoded.text = f"psrclr 0x{word & 0x1f:x}"
        return decoded
    if 0x0200 <= word <= 0x020F:
        decoded.text = f"pushn {reg(word & 0xf)}"
        return decoded
    if 0x0240 <= word <= 0x024F:
        decoded.text = f"popn {reg(word & 0xf)}"
        return decoded
    if word == 0x04C0:
        decoded.text = "reti"
        return decoded
    if word in (0x0640, 0x0740):
        decoded.text = f"ret{'.d' if word == 0x0740 else ''}"
        return decoded

    if (0x0600 <= word <= 0x060F) or (0x0700 <= word <= 0x070F):
        decoded.text = f"call{'.d' if (word >> 8) & 1 else ''} {reg(word & 0xf)}"
        return decoded
    if (0x0680 <= word <= 0x068F) or (0x0780 <= word <= 0x078F):
        decoded.text = f"jp{'.d' if (word >> 8) & 1 else ''} {reg(word & 0xf)}"
        return decoded

    if 0x0800 <= word <= 0x1BFF:
        op1 = (word >> 9) & 0xF
        if 4 <= op1 <= 13:
            dest = u32(pc + branch_disp(word, ext))
            decoded.text = f"{BRANCH_NAMES[op1]}{'.d' if (word >> 8) & 1 else ''} 0x{dest:08x}"
            decoded.comments.append(f"disp={branch_disp(word, ext)}")
            return decoded
    if 0x1C00 <= word <= 0x1DFF:
        dest = u32(pc + branch_disp(word, ext))
        decoded.text = f"call{'.d' if (word >> 8) & 1 else ''} 0x{dest:08x}"
        decoded.comments.append(f"disp={branch_disp(word, ext)}")
        decoded.call_targets.append(dest)
        return decoded
    if 0x1E00 <= word <= 0x1FFF:
        dest = u32(pc + branch_disp(word, ext))
        decoded.text = f"jp{'.d' if (word >> 8) & 1 else ''} 0x{dest:08x}"
        decoded.comments.append(f"disp={branch_disp(word, ext)}")
        return decoded

    if 0x2000 <= word <= 0x3DFF:
        op1 = (word >> 10) & 0x7
        op2 = (word >> 8) & 0x3
        rb = (word >> 4) & 0xF
        rd_rs = word & 0xF
        if op2 in (0, 1):
            suffix = "+" if op2 else ""
            if op1 <= 4:
                decoded.text = f"{LD_NAMES[op1]} {reg(rd_rs)}, [{reg(rb)}]{suffix}"
                if op2 == 0:
                    add_addr_comment(decoded, rb, ext, "R", (1, 1, 2, 2, 4)[op1], reg_consts)
            else:
                decoded.text = f"{LD_NAMES[op1]} [{reg(rb)}]{suffix}, {reg(rd_rs)}"
                if op2 == 0:
                    add_addr_comment(decoded, rb, ext, "W", (1, 2, 4)[op1 - 5], reg_consts)
            return decoded

    if 0x2200 <= word <= 0x3EFF and ((word >> 8) & 0x3) == 2:
        op1 = (word >> 10) & 0x7
        rs = (word >> 4) & 0xF
        rd = word & 0xF
        decoded.text = f"{REG_OP_NAMES[op1]} {reg(rd)}, {reg(rs)}"
        imm = ext_imm13_26(ext)
        if imm is not None:
            decoded.immediates.append(imm)
            decoded.comments.append(f"ext-imm=0x{imm:08x}")
        return decoded

    if 0x4000 <= word <= 0x5FFF:
        op1 = (word >> 10) & 0x7
        rd_rs = word & 0xF
        size = (1, 1, 2, 2, 4, 1, 2, 4)[op1]
        if op1 <= 4:
            decoded.text = f"{LD_NAMES[op1]} {reg(rd_rs)}, [%sp+0x{(word >> 4) & 0x3f:x}]"
            add_sp_comment(decoded, word, ext, size, "R")
        else:
            decoded.text = f"{LD_NAMES[op1]} [%sp+0x{(word >> 4) & 0x3f:x}], {reg(rd_rs)}"
            add_sp_comment(decoded, word, ext, size, "W")
        return decoded

    if (
        0xA800 <= word <= 0xB4F7
        and ((word >> 8) & 0x3) == 0
        and 2 <= ((word >> 10) & 0x7) <= 5
        and (word & 0x8) == 0
    ):
        op1 = (word >> 10) & 0x7
        rb = (word >> 4) & 0xF
        bit = word & 0x7
        decoded.text = f"{BITOP_NAMES[op1]} [{reg(rb)}], {bit}"
        access = "R" if op1 == 2 else "W"
        add_addr_comment(decoded, rb, ext, access, 1, reg_consts)
        return decoded

    if 0xA100 <= word <= 0xADFF and ((word >> 8) & 0x3) == 1:
        op1 = (word >> 10) & 0x7
        if op1 <= 3:
            decoded.text = f"{TRANSFER_NAMES[op1]} {reg(word & 0xf)}, {reg((word >> 4) & 0xf)}"
            return decoded

    if 0xA200 <= word <= 0xAEFF and ((word >> 8) & 0x3) == 2:
        op1 = (word >> 10) & 0x7
        if op1 <= 3:
            decoded.text = f"{MUL_NAMES[op1]} {reg(word & 0xf)}, {reg((word >> 4) & 0xf)}"
            return decoded

    if word & 0xFF00 in (0xB800, 0xBC00):
        op = "adc" if word & 0xFF00 == 0xB800 else "sbc"
        decoded.text = f"{op} {reg(word & 0xf)}, {reg((word >> 4) & 0xf)}"
        return decoded

    if 0x6000 <= word <= 0x63FF:
        imm = ext_imm6(word, ext, signed=False)
        decoded.text = f"add {reg(word & 0xf)}, 0x{imm:x}"
        decoded.immediates.append(imm)
        return decoded
    if 0x6400 <= word <= 0x67FF:
        imm = ext_imm6(word, ext, signed=False)
        decoded.text = f"sub {reg(word & 0xf)}, 0x{imm:x}"
        decoded.immediates.append(imm)
        return decoded
    if 0x8000 <= word <= 0x83FF:
        decoded.text = f"add %sp, 0x{word & 0x3ff:x}"
        return decoded
    if 0x8400 <= word <= 0x87FF:
        decoded.text = f"sub %sp, 0x{word & 0x3ff:x}"
        return decoded

    if 0x8800 <= word <= 0x9DFF and ((word >> 8) & 0x3) <= 1:
        op1 = (word >> 10) & 0x7
        op2 = (word >> 8) & 0x3
        if op1 in SHIFT_NAMES:
            if op2 == 0:
                decoded.text = f"{SHIFT_NAMES[op1]} {reg(word & 0xf)}, 0x{(word >> 4) & 0xf:x}"
            else:
                decoded.text = f"{SHIFT_NAMES[op1]} {reg(word & 0xf)}, {reg((word >> 4) & 0xf)}"
            return decoded

    if word & 0xFF00 in (0x9200, 0x9A00):
        decoded.text = (
            f"{'swap' if word & 0xFF00 == 0x9200 else 'swaph'} "
            f"{reg(word & 0xf)}, {reg((word >> 4) & 0xf)}"
        )
        return decoded

    if 0x8A00 <= word <= 0x8EFF and ((word >> 8) & 0x3) == 2:
        op1 = (word >> 10) & 0x7
        if op1 in SCAN_NAMES:
            decoded.text = f"{SCAN_NAMES[op1]} {reg(word & 0xf)}, {reg((word >> 4) & 0xf)}"
            return decoded

    if (
        0x8B00 <= word <= 0x9BF0
        and ((word >> 8) & 0x3) == 3
        and 2 <= ((word >> 10) & 0x7) <= 6
        and (word & 0xF) == 0
    ):
        op1 = (word >> 10) & 0x7
        decoded.text = DIV_NAMES[op1] if op1 == 6 else f"{DIV_NAMES[op1]} {reg((word >> 4) & 0xf)}"
        return decoded

    for start, end, name, signed in (
        (0x6800, 0x6BFF, "cmp", True),
        (0x6C00, 0x6FFF, "ld.w", True),
        (0x7000, 0x73FF, "and", True),
        (0x7400, 0x77FF, "or", True),
        (0x7800, 0x7BFF, "xor", True),
        (0x7C00, 0x7FFF, "not", True),
    ):
        if start <= word <= end:
            imm = ext_imm6(word, ext, signed=signed)
            decoded.text = f"{name} {reg(word & 0xf)}, 0x{u32(imm):08x}"
            decoded.immediates.append(u32(imm))
            return decoded

    if 0xA000 <= word <= 0xA0F3 and (word & 0xC) == 0:
        decoded.text = f"ld.w {sreg(word & 0xf)}, {reg((word >> 4) & 0xf)}"
        return decoded
    if 0xA400 <= word <= 0xA43F:
        decoded.text = f"ld.w {reg(word & 0xf)}, {sreg((word >> 4) & 0xf)}"
        return decoded

    return decoded


def insn_size_for_op1(op1: int) -> int:
    return (1, 1, 2, 2, 4, 1, 2, 4)[op1]


def clear_reg(reg_consts: dict[int, int], index: int) -> None:
    reg_consts.pop(index, None)


def update_reg_consts(reg_consts: dict[int, int], word: int, ext: list[int]) -> None:
    if 0x2000 <= word <= 0x3DFF:
        op1 = (word >> 10) & 0x7
        op2 = (word >> 8) & 0x3
        rb = (word >> 4) & 0xF
        rd_rs = word & 0xF
        if op2 in (0, 1):
            if op1 <= 4:
                clear_reg(reg_consts, rd_rs)
            if op2 == 1 and rb in reg_consts:
                reg_consts[rb] = u32(reg_consts[rb] + insn_size_for_op1(op1))
            return
        if op2 == 2:
            rs = rb
            rd = rd_rs
            if op1 == 2:
                return
            if op1 == 3 and rs in reg_consts:
                reg_consts[rd] = reg_consts[rs]
            else:
                clear_reg(reg_consts, rd)
            return

    if 0x4000 <= word <= 0x5FFF:
        op1 = (word >> 10) & 0x7
        if op1 <= 4:
            clear_reg(reg_consts, word & 0xF)
        return

    if 0xA100 <= word <= 0xADFF and ((word >> 8) & 0x3) == 1:
        op1 = (word >> 10) & 0x7
        rd = word & 0xF
        if op1 <= 3:
            clear_reg(reg_consts, rd)
        return

    if 0xA200 <= word <= 0xAEFF and ((word >> 8) & 0x3) == 2:
        op1 = (word >> 10) & 0x7
        rd = word & 0xF
        if op1 <= 3:
            clear_reg(reg_consts, rd)
        return

    if 0x6000 <= word <= 0x63FF:
        rd = word & 0xF
        if rd in reg_consts:
            reg_consts[rd] = u32(reg_consts[rd] + ext_imm6(word, ext, signed=False))
        return
    if 0x6400 <= word <= 0x67FF:
        rd = word & 0xF
        if rd in reg_consts:
            reg_consts[rd] = u32(reg_consts[rd] - ext_imm6(word, ext, signed=False))
        return

    if 0x6C00 <= word <= 0x6FFF:
        reg_consts[word & 0xF] = u32(ext_imm6(word, ext, signed=True))
        return

    if 0x7000 <= word <= 0x7FFF:
        rd = word & 0xF
        if 0x6800 <= word <= 0x6BFF:
            return
        if rd not in reg_consts:
            return
        imm = u32(ext_imm6(word, ext, signed=True))
        if 0x7000 <= word <= 0x73FF:
            reg_consts[rd] &= imm
        elif 0x7400 <= word <= 0x77FF:
            reg_consts[rd] |= imm
        elif 0x7800 <= word <= 0x7BFF:
            reg_consts[rd] ^= imm
        elif 0x7C00 <= word <= 0x7FFF:
            reg_consts[rd] = u32(~imm)
        return

    if 0xA400 <= word <= 0xA43F:
        clear_reg(reg_consts, word & 0xF)


def decode(data: bytes, base: int) -> list[Decoded]:
    decoded: list[Decoded] = []
    ext: list[int] = []
    ext_words: list[int] = []
    reg_consts: dict[int, int] = {}

    for offset in range(0, len(data) - 1, 2):
        pc = base + offset
        word = hword(data, offset)
        if (word & 0xE000) == 0xC000:
            imm13 = word & 0x1FFF
            if len(ext) == 1:
                ext = [ext[0], imm13]
                ext_words.append(word)
            else:
                ext = [imm13]
                ext_words = [word]
            decoded.append(Decoded(pc=pc, words=[word], text=f"ext 0x{imm13:x}"))
            continue

        used_words = ext_words + [word] if ext_words else [word]
        insn = decode_normal(pc, used_words, ext, reg_consts)
        if ext:
            insn.comments.insert(0, "ext=" + ",".join(f"0x{x:x}" for x in ext))
        decoded.append(insn)
        update_reg_consts(reg_consts, word, ext)
        ext = []
        ext_words = []

    return decoded


def load_image(path: Path) -> bytes:
    raw = path.read_bytes()
    if raw[:4] == b"KNL ":
        return raw[KERNEL_HEADER_SIZE:]
    return raw


def in_range(value: int, start: int | None, end: int | None) -> bool:
    if start is not None and value < start:
        return False
    if end is not None and value > end:
        return False
    return True


def format_line(insn: Decoded) -> str:
    words = " ".join(f"{word:04x}" for word in insn.words)
    line = f"0x{insn.pc:08x}: {words:<18} {insn.text}"
    if insn.comments:
        line += " ; " + "; ".join(insn.comments)
    return line


def matches_find(insn: Decoded, addrs: set[int], access: str) -> bool:
    if not addrs:
        return True
    for ref in insn.refs:
        if ref.addr in addrs and (access == "any" or ref.access.lower() == access):
            return True
    if access != "any":
        return False
    return any(value in addrs for value in insn.immediates)


def select_indexes(
    decoded: list[Decoded],
    args: argparse.Namespace,
) -> list[int]:
    find_addrs = set(args.find_addr)
    find_calls = set(args.find_call)
    indexes: set[int] = set()

    if find_addrs or find_calls:
        for i, insn in enumerate(decoded):
            addr_match = bool(find_addrs) and matches_find(
                insn, find_addrs, args.access
            )
            call_match = any(target in find_calls for target in insn.call_targets)
            if addr_match or call_match:
                for j in range(max(0, i - args.context), min(len(decoded), i + args.context + 1)):
                    indexes.add(j)
        return sorted(indexes)

    for i, insn in enumerate(decoded):
        if in_range(insn.pc, args.start, args.end):
            indexes.add(i)
    return sorted(indexes)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", default="build/bbk9288s-test/kernel.bin")
    parser.add_argument("--load-base", type=parse_u32, default=DEFAULT_LOAD_BASE)
    parser.add_argument("--start", type=parse_u32)
    parser.add_argument("--end", type=parse_u32)
    parser.add_argument("--find-addr", type=parse_u32, action="append", default=[])
    parser.add_argument(
        "--find-call",
        type=parse_u32,
        action="append",
        default=[],
        help="find direct call/call.d instructions targeting this address",
    )
    parser.add_argument("--access", choices=["any", "r", "w"], default="any")
    parser.add_argument("--context", type=int, default=0)
    parser.add_argument("--limit", type=int, default=400)
    args = parser.parse_args()

    data = load_image(Path(args.kernel))
    decoded = decode(data, args.load_base)
    indexes = select_indexes(decoded, args)

    print(f"decoded: {len(decoded)}")
    print(f"selected: {len(indexes)}")
    for i in indexes[: args.limit]:
        print(format_line(decoded[i]))
    if len(indexes) > args.limit:
        print(f"... {len(indexes) - args.limit} more")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
