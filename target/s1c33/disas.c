/*
 * Epson S1C33 disassembler preview.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "disas/dis-asm.h"
#include "cpu.h"

static const char *s1c33_sreg(unsigned reg)
{
    static const char * const names[] = {
        "psr", "sp", "alr", "ahr",
    };

    return reg < ARRAY_SIZE(names) ? names[reg] : "reserved";
}

static const char *s1c33_ld_name(unsigned op1)
{
    static const char * const names[] = {
        "ld.b", "ld.ub", "ld.h", "ld.uh", "ld.w", "ld.b", "ld.h", "ld.w",
    };

    return names[op1];
}

static const char *s1c33_reg_op_name(unsigned op1)
{
    static const char * const names[] = {
        "add", "sub", "cmp", "ld.w", "and", "or", "xor", "not",
    };

    return names[op1];
}

static const char *s1c33_branch_name(unsigned op1)
{
    static const char * const names[] = {
        [4] = "jrgt", [5] = "jrge", [6] = "jrlt", [7] = "jrle",
        [8] = "jrugt", [9] = "jruge", [10] = "jrult", [11] = "jrule",
        [12] = "jreq", [13] = "jrne",
    };

    return names[op1];
}

static const char *s1c33_bitop_name(unsigned op1)
{
    static const char * const names[] = {
        [2] = "btst", [3] = "bclr", [4] = "bset", [5] = "bnot",
    };

    return names[op1];
}

static const char *s1c33_class5_transfer_name(unsigned op1)
{
    static const char * const names[] = {
        "ld.b", "ld.ub", "ld.h", "ld.uh",
    };

    return names[op1];
}

static const char *s1c33_class4_shift_name(unsigned op1)
{
    static const char * const names[] = {
        [2] = "srl", [3] = "sll", [4] = "sra",
        [5] = "sla", [6] = "rr", [7] = "rl",
    };

    return names[op1];
}

static const char *s1c33_class4_scan_name(unsigned op1)
{
    static const char * const names[] = {
        [2] = "scan0", [3] = "scan1",
    };

    return names[op1];
}

static const char *s1c33_class4_div_name(unsigned op1)
{
    static const char * const names[] = {
        [2] = "div0s", [3] = "div0u", [4] = "div1",
        [5] = "div2s", [6] = "div3s",
    };

    return names[op1];
}

static const char *s1c33_class5_multiply_name(unsigned op1)
{
    static const char * const names[] = {
        "mlt.h", "mltu.h", "mlt.w", "mltu.w",
    };

    return names[op1];
}

static const char *s1c33_name(uint16_t word, char *buf, size_t len)
{
    unsigned op1;
    unsigned op2;
    unsigned rb_rs;
    unsigned rd_rs;

    if ((word & 0xe000) == 0xc000) {
        snprintf(buf, len, "ext 0x%x", word & 0x1fff);
        return buf;
    }
    if (word == 0x0000) {
        return "nop";
    }
    if (word == 0x0400) {
        return "brk";
    }
    if (word == 0x0040) {
        return "slp";
    }
    if (word == 0x0080) {
        return "halt";
    }
    if (word >= 0xbf40 && word <= 0xbf5f) {
        snprintf(buf, len, "psrset 0x%x", word & 0x1f);
        return buf;
    }
    if (word >= 0xbf60 && word <= 0xbf7f) {
        snprintf(buf, len, "psrclr 0x%x", word & 0x1f);
        return buf;
    }
    if (word >= 0x0200 && word <= 0x020f) {
        snprintf(buf, len, "pushn %%r%u", word & 0xf);
        return buf;
    }
    if (word >= 0x0240 && word <= 0x024f) {
        snprintf(buf, len, "popn %%r%u", word & 0xf);
        return buf;
    }
    if (word == 0x04c0) {
        return "reti";
    }
    if (word == 0x0640 || word == 0x0740) {
        snprintf(buf, len, "ret%s", word == 0x0740 ? ".d" : "");
        return buf;
    }
    if ((word >= 0x0600 && word <= 0x060f) ||
        (word >= 0x0700 && word <= 0x070f)) {
        snprintf(buf, len, "call%s %%r%u",
                 extract32(word, 8, 1) ? ".d" : "", word & 0xf);
        return buf;
    }
    if ((word >= 0x0680 && word <= 0x068f) ||
        (word >= 0x0780 && word <= 0x078f)) {
        snprintf(buf, len, "jp%s %%r%u",
                 extract32(word, 8, 1) ? ".d" : "", word & 0xf);
        return buf;
    }
    if (word >= 0x0800 && word <= 0x1bff) {
        op1 = extract32(word, 9, 4);
        if (op1 >= 4 && op1 <= 13) {
            snprintf(buf, len, "%s%s %d", s1c33_branch_name(op1),
                     extract32(word, 8, 1) ? ".d" : "",
                     sextract32(word, 0, 8) * 2);
            return buf;
        }
    }
    if (word >= 0x1c00 && word <= 0x1dff) {
        snprintf(buf, len, "call%s %d", extract32(word, 8, 1) ? ".d" : "",
                 sextract32(word, 0, 8) * 2);
        return buf;
    }
    if (word >= 0x1e00 && word <= 0x1fff) {
        snprintf(buf, len, "jp%s %d", extract32(word, 8, 1) ? ".d" : "",
                 sextract32(word, 0, 8) * 2);
        return buf;
    }
    if (word >= 0x2000 && word <= 0x3dff) {
        op1 = extract32(word, 10, 3);
        op2 = extract32(word, 8, 2);
        rb_rs = extract32(word, 4, 4);
        rd_rs = word & 0xf;
        if (op2 == 0 || op2 == 1) {
            if (op1 <= 4) {
                snprintf(buf, len, "%s %%r%u, [%%r%u]%s",
                         s1c33_ld_name(op1), rd_rs, rb_rs,
                         op2 ? "+" : "");
            } else {
                snprintf(buf, len, "%s [%%r%u]%s, %%r%u",
                         s1c33_ld_name(op1), rb_rs, op2 ? "+" : "", rd_rs);
            }
            return buf;
        }
    }
    if (word >= 0x2200 && word <= 0x3eff && extract32(word, 8, 2) == 2) {
        op1 = extract32(word, 10, 3);
        snprintf(buf, len, "%s %%r%u, %%r%u", s1c33_reg_op_name(op1),
                 word & 0xf, extract32(word, 4, 4));
        return buf;
    }
    if (word >= 0x4000 && word <= 0x5fff) {
        op1 = extract32(word, 10, 3);
        if (op1 <= 4) {
            snprintf(buf, len, "%s %%r%u, [%%sp+0x%x]",
                     s1c33_ld_name(op1), word & 0xf,
                     extract32(word, 4, 6));
        } else {
            snprintf(buf, len, "%s [%%sp+0x%x], %%r%u",
                     s1c33_ld_name(op1), extract32(word, 4, 6),
                     word & 0xf);
        }
        return buf;
    }
    if ((word >= 0xa800 && word <= 0xb4f7) &&
        extract32(word, 8, 2) == 0 &&
        extract32(word, 10, 3) >= 2 &&
        extract32(word, 10, 3) <= 5 &&
        (word & 0x8) == 0) {
        op1 = extract32(word, 10, 3);
        snprintf(buf, len, "%s [%%r%u], %u", s1c33_bitop_name(op1),
                 extract32(word, 4, 4), extract32(word, 0, 3));
        return buf;
    }
    if (word >= 0xa100 && word <= 0xadff &&
        extract32(word, 8, 2) == 1 &&
        extract32(word, 10, 3) <= 3) {
        op1 = extract32(word, 10, 3);
        snprintf(buf, len, "%s %%r%u, %%r%u",
                 s1c33_class5_transfer_name(op1), word & 0xf,
                 extract32(word, 4, 4));
        return buf;
    }
    if (word >= 0xa200 && word <= 0xaeff &&
        extract32(word, 8, 2) == 2 &&
        extract32(word, 10, 3) <= 3) {
        op1 = extract32(word, 10, 3);
        snprintf(buf, len, "%s %%r%u, %%r%u",
                 s1c33_class5_multiply_name(op1), word & 0xf,
                 extract32(word, 4, 4));
        return buf;
    }
    if (word >= 0x6000 && word <= 0x63ff) {
        snprintf(buf, len, "add %%r%u, 0x%x", word & 0xf,
                 (word >> 4) & 0x3f);
        return buf;
    }
    if (word >= 0x6400 && word <= 0x67ff) {
        snprintf(buf, len, "sub %%r%u, 0x%x", word & 0xf,
                 (word >> 4) & 0x3f);
        return buf;
    }
    if (word >= 0x8000 && word <= 0x83ff) {
        snprintf(buf, len, "add %%sp, 0x%x", word & 0x3ff);
        return buf;
    }
    if (word >= 0x8400 && word <= 0x87ff) {
        snprintf(buf, len, "sub %%sp, 0x%x", word & 0x3ff);
        return buf;
    }
    if (word >= 0x8800 && word <= 0x9dff &&
        extract32(word, 8, 2) <= 1) {
        op1 = extract32(word, 10, 3);
        op2 = extract32(word, 8, 2);
        if (op2 == 0) {
            snprintf(buf, len, "%s %%r%u, 0x%x",
                     s1c33_class4_shift_name(op1), word & 0xf,
                     extract32(word, 4, 4));
        } else {
            snprintf(buf, len, "%s %%r%u, %%r%u",
                     s1c33_class4_shift_name(op1), word & 0xf,
                     extract32(word, 4, 4));
        }
        return buf;
    }
    if (word >= 0x8a00 && word <= 0x8eff &&
        extract32(word, 8, 2) == 2 &&
        extract32(word, 10, 3) >= 2 &&
        extract32(word, 10, 3) <= 3) {
        op1 = extract32(word, 10, 3);
        snprintf(buf, len, "%s %%r%u, %%r%u",
                 s1c33_class4_scan_name(op1), word & 0xf,
                 extract32(word, 4, 4));
        return buf;
    }
    if (word >= 0x8b00 && word <= 0x9bf0 &&
        extract32(word, 8, 2) == 3 &&
        extract32(word, 10, 3) >= 2 &&
        extract32(word, 10, 3) <= 6 &&
        (word & 0xf) == 0) {
        op1 = extract32(word, 10, 3);
        if (op1 == 6) {
            snprintf(buf, len, "%s", s1c33_class4_div_name(op1));
        } else {
            snprintf(buf, len, "%s %%r%u", s1c33_class4_div_name(op1),
                     extract32(word, 4, 4));
        }
        return buf;
    }
    if (word >= 0x6800 && word <= 0x6bff) {
        snprintf(buf, len, "cmp %%r%u, 0x%x", word & 0xf,
                 (uint32_t)sextract32(word >> 4, 0, 6));
        return buf;
    }
    if (word >= 0x6c00 && word <= 0x6fff) {
        snprintf(buf, len, "ld.w %%r%u, 0x%x", word & 0xf,
                 (uint32_t)sextract32(word >> 4, 0, 6));
        return buf;
    }
    if (word >= 0x7000 && word <= 0x73ff) {
        snprintf(buf, len, "and %%r%u, 0x%x", word & 0xf,
                 (uint32_t)sextract32(word >> 4, 0, 6));
        return buf;
    }
    if (word >= 0x7400 && word <= 0x77ff) {
        snprintf(buf, len, "or %%r%u, 0x%x", word & 0xf,
                 (uint32_t)sextract32(word >> 4, 0, 6));
        return buf;
    }
    if (word >= 0x7800 && word <= 0x7bff) {
        snprintf(buf, len, "xor %%r%u, 0x%x", word & 0xf,
                 (uint32_t)sextract32(word >> 4, 0, 6));
        return buf;
    }
    if (word >= 0x7c00 && word <= 0x7fff) {
        snprintf(buf, len, "not %%r%u, 0x%x", word & 0xf,
                 (uint32_t)sextract32(word >> 4, 0, 6));
        return buf;
    }
    if (word >= 0xa000 && word <= 0xa0f3 && (word & 0xc) == 0) {
        snprintf(buf, len, "ld.w %%%s, %%r%u", s1c33_sreg(word & 0xf),
                 extract32(word, 4, 4));
        return buf;
    }
    if (word >= 0xa400 && word <= 0xa43f) {
        snprintf(buf, len, "ld.w %%r%u, %%%s", word & 0xf,
                 s1c33_sreg(extract32(word, 4, 4)));
        return buf;
    }
    if ((word & 0xff00) == 0xb800 || (word & 0xff00) == 0xbc00) {
        snprintf(buf, len, "%s %%r%u, %%r%u",
                 (word & 0xff00) == 0xb800 ? "adc" : "sbc",
                 word & 0xf, extract32(word, 4, 4));
        return buf;
    }
    snprintf(buf, len, ".hword 0x%04x", word);
    return buf;
}

int print_insn_s1c33(bfd_vma addr, disassemble_info *info)
{
    uint8_t bytes[2];
    uint16_t word;
    char buf[32];

    if (info->read_memory_func(addr, bytes, sizeof(bytes), info) != 0) {
        info->memory_error_func(-1, addr, info);
        return -1;
    }

    word = lduw_le_p(bytes);
    info->fprintf_func(info->stream, "%04x\t%s",
                       word, s1c33_name(word, buf, sizeof(buf)));
    return 2;
}
