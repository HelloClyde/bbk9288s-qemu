/*
 * Epson S1C33 TCG translation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-gen.h"
#include "exec/helper-proto.h"
#include "exec/log.h"
#include "exec/translator.h"
#include "exec/translation-block.h"
#include "tcg/tcg-op.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

typedef struct DisasContext {
    DisasContextBase base;
    CPUS1C33State *env;
    uint32_t pc;
    uint16_t ext[2];
    unsigned ext_count;
} DisasContext;

enum {
    S1C33_TRACE_CALL,
    S1C33_TRACE_REG_CALL,
    S1C33_TRACE_RET,
    S1C33_TRACE_RETI,
};

static void translate_one(DisasContext *ctx, uint16_t word);

static TCGv_i32 cpu_regs[S1C33_NUM_REGS];
static TCGv_i32 cpu_pc;
static TCGv_i32 cpu_psr;
static TCGv_i32 cpu_sp;
static TCGv_i32 cpu_alr;
static TCGv_i32 cpu_ahr;
static TCGv_i32 cpu_ext[2];
static TCGv_i32 cpu_ext_count;

static uint32_t s1c33_sign6(uint16_t word)
{
    return sextract32(word >> 4, 0, 6);
}

static uint32_t s1c33_imm6(uint16_t word)
{
    return (word >> 4) & 0x3f;
}

static void gen_clear_ext(DisasContext *ctx)
{
    ctx->ext_count = 0;
    tcg_gen_movi_i32(cpu_ext_count, 0);
}

static bool trace_mem_enabled(DisasContext *ctx)
{
    return env_archcpu(ctx->env)->trace_mem;
}

static void gen_trace_mem(DisasContext *ctx, bool is_write, TCGv_i32 addr,
                          TCGv_i32 value, unsigned size)
{
    if (!trace_mem_enabled(ctx)) {
        return;
    }

    gen_helper_s1c33_trace_mem(tcg_env, tcg_constant_i32(is_write),
                               tcg_constant_i32(ctx->pc), addr, value,
                               tcg_constant_i32(size));
}

static TCGv_i32 s1c33_special_reg(unsigned reg)
{
    switch (reg) {
    case 0:
        return cpu_psr;
    case 1:
        return cpu_sp;
    case 2:
        return cpu_alr;
    case 3:
        return cpu_ahr;
    default:
        return NULL;
    }
}

static void gen_extended_sign6(TCGv_i32 dest, uint16_t word)
{
    TCGLabel *standard = gen_new_label();
    TCGLabel *one_ext = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv_i32 count = tcg_temp_new_i32();
    TCGv_i32 tmp = tcg_temp_new_i32();

    tcg_gen_mov_i32(count, cpu_ext_count);
    tcg_gen_brcondi_i32(TCG_COND_EQ, count, 0, standard);
    tcg_gen_brcondi_i32(TCG_COND_EQ, count, 1, one_ext);

    tcg_gen_shli_i32(dest, cpu_ext[0], 19);
    tcg_gen_shli_i32(tmp, cpu_ext[1], 6);
    tcg_gen_or_i32(dest, dest, tmp);
    tcg_gen_ori_i32(dest, dest, (word >> 4) & 0x3f);
    tcg_gen_br(done);

    gen_set_label(one_ext);
    tcg_gen_shli_i32(dest, cpu_ext[0], 6);
    tcg_gen_ori_i32(dest, dest, (word >> 4) & 0x3f);
    tcg_gen_shli_i32(dest, dest, 13);
    tcg_gen_sari_i32(dest, dest, 13);
    tcg_gen_br(done);

    gen_set_label(standard);
    tcg_gen_movi_i32(dest, s1c33_sign6(word));

    gen_set_label(done);
}

static void gen_extended_imm6(TCGv_i32 dest, uint16_t word)
{
    TCGLabel *standard = gen_new_label();
    TCGLabel *one_ext = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv_i32 count = tcg_temp_new_i32();
    TCGv_i32 tmp = tcg_temp_new_i32();

    tcg_gen_mov_i32(count, cpu_ext_count);
    tcg_gen_brcondi_i32(TCG_COND_EQ, count, 0, standard);
    tcg_gen_brcondi_i32(TCG_COND_EQ, count, 1, one_ext);

    tcg_gen_shli_i32(dest, cpu_ext[0], 19);
    tcg_gen_shli_i32(tmp, cpu_ext[1], 6);
    tcg_gen_or_i32(dest, dest, tmp);
    tcg_gen_ori_i32(dest, dest, s1c33_imm6(word));
    tcg_gen_br(done);

    gen_set_label(one_ext);
    tcg_gen_shli_i32(dest, cpu_ext[0], 6);
    tcg_gen_ori_i32(dest, dest, s1c33_imm6(word));
    tcg_gen_br(done);

    gen_set_label(standard);
    tcg_gen_movi_i32(dest, s1c33_imm6(word));

    gen_set_label(done);
}

static void gen_extended_addr(TCGv_i32 addr, TCGv_i32 base)
{
    TCGLabel *standard = gen_new_label();
    TCGLabel *one_ext = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv_i32 count = tcg_temp_new_i32();

    tcg_gen_mov_i32(count, cpu_ext_count);
    tcg_gen_brcondi_i32(TCG_COND_EQ, count, 0, standard);
    tcg_gen_brcondi_i32(TCG_COND_EQ, count, 1, one_ext);

    tcg_gen_shli_i32(addr, cpu_ext[0], 13);
    tcg_gen_or_i32(addr, addr, cpu_ext[1]);
    tcg_gen_add_i32(addr, addr, base);
    tcg_gen_br(done);

    gen_set_label(one_ext);
    tcg_gen_add_i32(addr, base, cpu_ext[0]);
    tcg_gen_br(done);

    gen_set_label(standard);
    tcg_gen_mov_i32(addr, base);

    gen_set_label(done);
}

static void gen_extended_imm13_26(TCGv_i32 dest)
{
    TCGLabel *one_ext = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv_i32 count = tcg_temp_new_i32();

    tcg_gen_mov_i32(count, cpu_ext_count);
    tcg_gen_brcondi_i32(TCG_COND_EQ, count, 1, one_ext);

    tcg_gen_shli_i32(dest, cpu_ext[0], 13);
    tcg_gen_or_i32(dest, dest, cpu_ext[1]);
    tcg_gen_br(done);

    gen_set_label(one_ext);
    tcg_gen_mov_i32(dest, cpu_ext[0]);

    gen_set_label(done);
}

static void gen_set_nz(TCGv_i32 result)
{
    TCGv_i32 z = tcg_temp_new_i32();
    TCGv_i32 n = tcg_temp_new_i32();

    tcg_gen_setcondi_i32(TCG_COND_EQ, z, result, 0);
    tcg_gen_shri_i32(n, result, 31);
    tcg_gen_andi_i32(cpu_psr, cpu_psr, ~3u);
    tcg_gen_or_i32(cpu_psr, cpu_psr, n);
    tcg_gen_shli_i32(z, z, 1);
    tcg_gen_or_i32(cpu_psr, cpu_psr, z);
}

static void gen_set_add_flags(TCGv_i32 lhs, TCGv_i32 rhs, TCGv_i32 result)
{
    TCGv_i32 n = tcg_temp_new_i32();
    TCGv_i32 z = tcg_temp_new_i32();
    TCGv_i32 v = tcg_temp_new_i32();
    TCGv_i32 c = tcg_temp_new_i32();
    TCGv_i32 tmp = tcg_temp_new_i32();

    tcg_gen_shri_i32(n, result, 31);
    tcg_gen_setcondi_i32(TCG_COND_EQ, z, result, 0);
    tcg_gen_xor_i32(v, lhs, result);
    tcg_gen_xor_i32(tmp, rhs, result);
    tcg_gen_and_i32(v, v, tmp);
    tcg_gen_shri_i32(v, v, 31);
    tcg_gen_setcond_i32(TCG_COND_LTU, c, result, lhs);

    tcg_gen_andi_i32(cpu_psr, cpu_psr, ~0xfu);
    tcg_gen_or_i32(cpu_psr, cpu_psr, n);
    tcg_gen_shli_i32(z, z, 1);
    tcg_gen_or_i32(cpu_psr, cpu_psr, z);
    tcg_gen_shli_i32(v, v, 2);
    tcg_gen_or_i32(cpu_psr, cpu_psr, v);
    tcg_gen_shli_i32(c, c, 3);
    tcg_gen_or_i32(cpu_psr, cpu_psr, c);
}

static void gen_set_sub_flags(TCGv_i32 lhs, TCGv_i32 rhs, TCGv_i32 result)
{
    TCGv_i32 n = tcg_temp_new_i32();
    TCGv_i32 z = tcg_temp_new_i32();
    TCGv_i32 v = tcg_temp_new_i32();
    TCGv_i32 c = tcg_temp_new_i32();
    TCGv_i32 tmp = tcg_temp_new_i32();

    tcg_gen_shri_i32(n, result, 31);
    tcg_gen_setcondi_i32(TCG_COND_EQ, z, result, 0);
    tcg_gen_xor_i32(v, lhs, rhs);
    tcg_gen_xor_i32(tmp, lhs, result);
    tcg_gen_and_i32(v, v, tmp);
    tcg_gen_shri_i32(v, v, 31);
    tcg_gen_setcond_i32(TCG_COND_LTU, c, lhs, rhs);

    tcg_gen_andi_i32(cpu_psr, cpu_psr, ~0xfu);
    tcg_gen_or_i32(cpu_psr, cpu_psr, n);
    tcg_gen_shli_i32(z, z, 1);
    tcg_gen_or_i32(cpu_psr, cpu_psr, z);
    tcg_gen_shli_i32(v, v, 2);
    tcg_gen_or_i32(cpu_psr, cpu_psr, v);
    tcg_gen_shli_i32(c, c, 3);
    tcg_gen_or_i32(cpu_psr, cpu_psr, c);
}

static void gen_set_adc_flags(TCGv_i32 lhs, TCGv_i32 rhs,
                              TCGv_i32 carry_in, TCGv_i32 result)
{
    TCGv_i32 n = tcg_temp_new_i32();
    TCGv_i32 z = tcg_temp_new_i32();
    TCGv_i32 v = tcg_temp_new_i32();
    TCGv_i32 c = tcg_temp_new_i32();
    TCGv_i32 tmp = tcg_temp_new_i32();

    tcg_gen_shri_i32(n, result, 31);
    tcg_gen_setcondi_i32(TCG_COND_EQ, z, result, 0);
    tcg_gen_xor_i32(v, lhs, rhs);
    tcg_gen_not_i32(v, v);
    tcg_gen_xor_i32(tmp, lhs, result);
    tcg_gen_and_i32(v, v, tmp);
    tcg_gen_shri_i32(v, v, 31);

    tcg_gen_setcond_i32(TCG_COND_LTU, c, result, lhs);
    tcg_gen_setcond_i32(TCG_COND_EQ, tmp, result, lhs);
    tcg_gen_and_i32(tmp, tmp, carry_in);
    tcg_gen_or_i32(c, c, tmp);

    tcg_gen_andi_i32(cpu_psr, cpu_psr, ~0xfu);
    tcg_gen_or_i32(cpu_psr, cpu_psr, n);
    tcg_gen_shli_i32(z, z, 1);
    tcg_gen_or_i32(cpu_psr, cpu_psr, z);
    tcg_gen_shli_i32(v, v, 2);
    tcg_gen_or_i32(cpu_psr, cpu_psr, v);
    tcg_gen_shli_i32(c, c, 3);
    tcg_gen_or_i32(cpu_psr, cpu_psr, c);
}

static void gen_set_sbc_flags(TCGv_i32 lhs, TCGv_i32 rhs,
                              TCGv_i32 borrow_in, TCGv_i32 result)
{
    TCGv_i32 n = tcg_temp_new_i32();
    TCGv_i32 z = tcg_temp_new_i32();
    TCGv_i32 v = tcg_temp_new_i32();
    TCGv_i32 c = tcg_temp_new_i32();
    TCGv_i32 tmp = tcg_temp_new_i32();

    tcg_gen_shri_i32(n, result, 31);
    tcg_gen_setcondi_i32(TCG_COND_EQ, z, result, 0);
    tcg_gen_xor_i32(v, lhs, rhs);
    tcg_gen_xor_i32(tmp, lhs, result);
    tcg_gen_and_i32(v, v, tmp);
    tcg_gen_shri_i32(v, v, 31);

    tcg_gen_setcond_i32(TCG_COND_LTU, c, lhs, rhs);
    tcg_gen_setcond_i32(TCG_COND_EQ, tmp, lhs, rhs);
    tcg_gen_and_i32(tmp, tmp, borrow_in);
    tcg_gen_or_i32(c, c, tmp);

    tcg_gen_andi_i32(cpu_psr, cpu_psr, ~0xfu);
    tcg_gen_or_i32(cpu_psr, cpu_psr, n);
    tcg_gen_shli_i32(z, z, 1);
    tcg_gen_or_i32(cpu_psr, cpu_psr, z);
    tcg_gen_shli_i32(v, v, 2);
    tcg_gen_or_i32(cpu_psr, cpu_psr, v);
    tcg_gen_shli_i32(c, c, 3);
    tcg_gen_or_i32(cpu_psr, cpu_psr, c);
}

static void gen_goto_tb(DisasContext *ctx, unsigned n, vaddr dest)
{
    if (translator_use_goto_tb(&ctx->base, dest)) {
        tcg_gen_goto_tb(n);
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_exit_tb(ctx->base.tb, n);
    } else {
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_lookup_and_goto_ptr();
    }
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void gen_branch_tb(DisasContext *ctx, TCGv_i32 cond, vaddr dest)
{
    TCGLabel *fallthrough = gen_new_label();

    tcg_gen_brcondi_i32(TCG_COND_EQ, cond, 0, fallthrough);
    if (translator_use_goto_tb(&ctx->base, dest)) {
        tcg_gen_goto_tb(0);
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_exit_tb(ctx->base.tb, 0);
    } else {
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_lookup_and_goto_ptr();
    }

    gen_set_label(fallthrough);
    if (translator_use_goto_tb(&ctx->base, ctx->base.pc_next)) {
        tcg_gen_goto_tb(1);
        tcg_gen_movi_i32(cpu_pc, ctx->base.pc_next);
        tcg_gen_exit_tb(ctx->base.tb, 1);
    } else {
        tcg_gen_movi_i32(cpu_pc, ctx->base.pc_next);
        tcg_gen_lookup_and_goto_ptr();
    }
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void gen_unimplemented(DisasContext *ctx, uint16_t word)
{
    tcg_gen_movi_i32(cpu_pc, ctx->pc);
    gen_helper_s1c33_unimplemented(tcg_env, tcg_constant_i32(word));
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void gen_psr_bit(DisasContext *ctx, uint16_t word, bool set)
{
    unsigned bit = word & 0x1f;
    TCGv_i32 value = tcg_temp_new_i32();

    if (set) {
        tcg_gen_ori_i32(value, cpu_psr, 1u << bit);
    } else {
        tcg_gen_andi_i32(value, cpu_psr, ~(1u << bit));
    }
    gen_helper_s1c33_write_psr(tcg_env, value, tcg_constant_i32(ctx->pc),
                               tcg_constant_i32(word));
}

static int32_t s1c33_branch_disp(DisasContext *ctx, uint16_t word)
{
    uint32_t sign8 = word & 0xff;
    uint32_t disp;

    switch (ctx->ext_count) {
    case 0:
        return sextract32(sign8, 0, 8) * 2;
    case 1:
        disp = (ctx->ext[0] << 9) | (sign8 << 1);
        return sextract32(disp, 0, 22);
    default:
        disp = ((ctx->ext[0] >> 3) << 22) | (ctx->ext[1] << 9) |
               (sign8 << 1);
        return (int32_t)disp;
    }
}

static void gen_ext(DisasContext *ctx, uint16_t word)
{
    uint32_t imm13 = word & 0x1fff;
    TCGLabel *append = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv_i32 count = tcg_temp_new_i32();

    if (ctx->ext_count == 1) {
        ctx->ext[1] = imm13;
        ctx->ext_count = 2;
    } else {
        ctx->ext[0] = imm13;
        ctx->ext[1] = 0;
        ctx->ext_count = 1;
    }

    tcg_gen_mov_i32(count, cpu_ext_count);
    tcg_gen_brcondi_i32(TCG_COND_EQ, count, 1, append);

    tcg_gen_movi_i32(cpu_ext[0], imm13);
    tcg_gen_movi_i32(cpu_ext_count, 1);
    tcg_gen_br(done);

    gen_set_label(append);
    tcg_gen_movi_i32(cpu_ext[1], imm13);
    tcg_gen_movi_i32(cpu_ext_count, 2);

    gen_set_label(done);
}

static void gen_branch_condition(TCGv_i32 cond, unsigned op1)
{
    TCGv_i32 n = tcg_temp_new_i32();
    TCGv_i32 z = tcg_temp_new_i32();
    TCGv_i32 v = tcg_temp_new_i32();
    TCGv_i32 c = tcg_temp_new_i32();
    TCGv_i32 nv = tcg_temp_new_i32();

    tcg_gen_andi_i32(n, cpu_psr, 1);
    tcg_gen_shri_i32(z, cpu_psr, 1);
    tcg_gen_andi_i32(z, z, 1);
    tcg_gen_shri_i32(v, cpu_psr, 2);
    tcg_gen_andi_i32(v, v, 1);
    tcg_gen_shri_i32(c, cpu_psr, 3);
    tcg_gen_andi_i32(c, c, 1);
    tcg_gen_xor_i32(nv, n, v);

    switch (op1) {
    case 4: /* jrgt: !Z & !(N ^ V) */
        tcg_gen_or_i32(cond, z, nv);
        tcg_gen_setcondi_i32(TCG_COND_EQ, cond, cond, 0);
        break;
    case 5: /* jrge: !(N ^ V) */
        tcg_gen_setcondi_i32(TCG_COND_EQ, cond, nv, 0);
        break;
    case 6: /* jrlt: N ^ V */
        tcg_gen_mov_i32(cond, nv);
        break;
    case 7: /* jrle: Z | (N ^ V) */
        tcg_gen_or_i32(cond, z, nv);
        break;
    case 8: /* jrugt: !Z & !C */
        tcg_gen_or_i32(cond, z, c);
        tcg_gen_setcondi_i32(TCG_COND_EQ, cond, cond, 0);
        break;
    case 9: /* jruge: !C */
        tcg_gen_setcondi_i32(TCG_COND_EQ, cond, c, 0);
        break;
    case 10: /* jrult: C */
        tcg_gen_mov_i32(cond, c);
        break;
    case 11: /* jrule: Z | C */
        tcg_gen_or_i32(cond, z, c);
        break;
    case 12: /* jreq: Z */
        tcg_gen_mov_i32(cond, z);
        break;
    case 13: /* jrne: !Z */
        tcg_gen_setcondi_i32(TCG_COND_EQ, cond, z, 0);
        break;
    default:
        g_assert_not_reached();
    }
}

static void gen_cond_branch(DisasContext *ctx, uint16_t word)
{
    unsigned op1 = extract32(word, 9, 4);
    bool delayed = extract32(word, 8, 1);
    int32_t disp = s1c33_branch_disp(ctx, word);
    uint32_t dest = ctx->pc + disp;
    uint32_t delay_pc;
    uint16_t delay_word;
    TCGv_i32 cond = tcg_temp_new_i32();

    gen_branch_condition(cond, op1);
    gen_clear_ext(ctx);

    if (delayed) {
        delay_pc = ctx->base.pc_next;
        delay_word = translator_lduw_end(ctx->env, &ctx->base, delay_pc,
                                         MO_LE);
        ctx->base.pc_next += 2;
        ctx->pc = delay_pc;
        translate_one(ctx, delay_word);
        if (ctx->base.is_jmp != DISAS_NEXT) {
            return;
        }
    }

    gen_branch_tb(ctx, cond, dest);
}

static void gen_ret(DisasContext *ctx, uint16_t word)
{
    bool delayed = extract32(word, 8, 1);
    uint32_t delay_pc;
    uint16_t delay_word;
    TCGv_i32 target = tcg_temp_new_i32();

    tcg_gen_qemu_ld_i32(target, cpu_sp, 0, MO_LEUL);
    tcg_gen_addi_i32(cpu_sp, cpu_sp, 4);
    gen_clear_ext(ctx);
    gen_helper_s1c33_trace_branch(tcg_env, tcg_constant_i32(S1C33_TRACE_RET),
                                  tcg_constant_i32(ctx->pc), target,
                                  tcg_constant_i32(0));
    if (delayed) {
        delay_pc = ctx->base.pc_next;
        delay_word = translator_lduw_end(ctx->env, &ctx->base, delay_pc,
                                         MO_LE);
        ctx->base.pc_next += 2;
        ctx->pc = delay_pc;
        translate_one(ctx, delay_word);
        if (ctx->base.is_jmp != DISAS_NEXT) {
            return;
        }
    }

    tcg_gen_mov_i32(cpu_pc, target);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void gen_reti(DisasContext *ctx)
{
    TCGv_i32 psr = tcg_temp_new_i32();
    TCGv_i32 target = tcg_temp_new_i32();

    tcg_gen_qemu_ld_i32(psr, cpu_sp, 0, MO_LEUL);
    tcg_gen_addi_i32(cpu_sp, cpu_sp, 4);
    gen_helper_s1c33_write_psr(tcg_env, psr, tcg_constant_i32(ctx->pc),
                               tcg_constant_i32(0x04c0));
    tcg_gen_qemu_ld_i32(target, cpu_sp, 0, MO_LEUL);
    tcg_gen_addi_i32(cpu_sp, cpu_sp, 4);
    gen_clear_ext(ctx);
    gen_helper_s1c33_trace_branch(tcg_env, tcg_constant_i32(S1C33_TRACE_RETI),
                                  tcg_constant_i32(ctx->pc), target, psr);
    tcg_gen_mov_i32(cpu_pc, target);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void gen_relative_call_or_jump(DisasContext *ctx, uint16_t word,
                                      bool call)
{
    bool delayed = extract32(word, 8, 1);
    int32_t disp = s1c33_branch_disp(ctx, word);
    uint32_t dest = ctx->pc + disp;
    uint32_t return_pc = ctx->base.pc_next + (delayed ? 2 : 0);
    uint32_t delay_pc;
    uint16_t delay_word;

    gen_clear_ext(ctx);

    if (call) {
        tcg_gen_subi_i32(cpu_sp, cpu_sp, 4);
        tcg_gen_qemu_st_i32(tcg_constant_i32(return_pc), cpu_sp, 0,
                            MO_LEUL);
        gen_helper_s1c33_trace_branch(tcg_env,
                                      tcg_constant_i32(S1C33_TRACE_CALL),
                                      tcg_constant_i32(ctx->pc),
                                      tcg_constant_i32(dest),
                                      tcg_constant_i32(return_pc));
    }

    if (delayed) {
        delay_pc = ctx->base.pc_next;
        delay_word = translator_lduw_end(ctx->env, &ctx->base, delay_pc,
                                         MO_LE);
        ctx->base.pc_next += 2;
        ctx->pc = delay_pc;
        translate_one(ctx, delay_word);
        if (ctx->base.is_jmp != DISAS_NEXT) {
            return;
        }
    }

    gen_goto_tb(ctx, 0, dest);
}

static void gen_register_call_or_jump(DisasContext *ctx, uint16_t word,
                                      bool call)
{
    bool delayed = extract32(word, 8, 1);
    unsigned rb = word & 0xf;
    uint32_t return_pc = ctx->base.pc_next + (delayed ? 2 : 0);
    uint32_t delay_pc;
    uint16_t delay_word;
    TCGv_i32 target = tcg_temp_new_i32();

    tcg_gen_andi_i32(target, cpu_regs[rb], 0x0ffffffe);
    gen_clear_ext(ctx);

    if (call) {
        tcg_gen_subi_i32(cpu_sp, cpu_sp, 4);
        tcg_gen_qemu_st_i32(tcg_constant_i32(return_pc), cpu_sp, 0,
                            MO_LEUL);
        gen_helper_s1c33_trace_branch(tcg_env,
                                      tcg_constant_i32(S1C33_TRACE_REG_CALL),
                                      tcg_constant_i32(ctx->pc), target,
                                      tcg_constant_i32(return_pc));
    }

    if (delayed) {
        delay_pc = ctx->base.pc_next;
        delay_word = translator_lduw_end(ctx->env, &ctx->base, delay_pc,
                                         MO_LE);
        ctx->base.pc_next += 2;
        ctx->pc = delay_pc;
        translate_one(ctx, delay_word);
        if (ctx->base.is_jmp != DISAS_NEXT) {
            return;
        }
    }

    tcg_gen_mov_i32(cpu_pc, target);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void gen_ld_special_to_reg(DisasContext *ctx, uint16_t word)
{
    unsigned ss = extract32(word, 4, 4);
    unsigned rd = extract32(word, 0, 4);
    TCGv_i32 src = s1c33_special_reg(ss);

    if (src == NULL) {
        gen_clear_ext(ctx);
        return;
    }
    tcg_gen_mov_i32(cpu_regs[rd], src);
    gen_clear_ext(ctx);
}

static void gen_ld_reg_to_special(DisasContext *ctx, uint16_t word)
{
    unsigned rs = extract32(word, 4, 4);
    unsigned sd = extract32(word, 0, 4);
    TCGv_i32 dst = s1c33_special_reg(sd);

    if (dst == NULL) {
        gen_clear_ext(ctx);
        return;
    }
    if (sd == 0) {
        gen_helper_s1c33_write_psr(tcg_env, cpu_regs[rs],
                                   tcg_constant_i32(ctx->pc),
                                   tcg_constant_i32(word));
    } else {
        tcg_gen_mov_i32(dst, cpu_regs[rs]);
    }
    gen_clear_ext(ctx);
}

static void gen_pushn(DisasContext *ctx, uint16_t word)
{
    unsigned rs = word & 0xf;
    int r;

    for (r = rs; r >= 0; r--) {
        tcg_gen_subi_i32(cpu_sp, cpu_sp, 4);
        tcg_gen_qemu_st_i32(cpu_regs[r], cpu_sp, 0, MO_LEUL);
    }
    gen_clear_ext(ctx);
}

static void gen_popn(DisasContext *ctx, uint16_t word)
{
    unsigned rd = word & 0xf;
    unsigned r;

    for (r = 0; r <= rd; r++) {
        tcg_gen_qemu_ld_i32(cpu_regs[r], cpu_sp, 0, MO_LEUL);
        tcg_gen_addi_i32(cpu_sp, cpu_sp, 4);
    }
    gen_clear_ext(ctx);
}

static void gen_sp_adjust(DisasContext *ctx, uint16_t word, bool sub)
{
    uint32_t imm = (word & 0x3ff) * 4;

    if (sub) {
        tcg_gen_subi_i32(cpu_sp, cpu_sp, imm);
    } else {
        tcg_gen_addi_i32(cpu_sp, cpu_sp, imm);
    }
    gen_clear_ext(ctx);
}

static void gen_shift_count_reg(TCGv_i32 count, TCGv_i32 reg)
{
    TCGLabel *done = gen_new_label();
    TCGv_i32 high = tcg_temp_new_i32();

    tcg_gen_andi_i32(count, reg, 0xf);
    tcg_gen_andi_i32(high, count, 0x8);
    tcg_gen_brcondi_i32(TCG_COND_EQ, high, 0, done);
    tcg_gen_movi_i32(count, 8);

    gen_set_label(done);
}

static unsigned s1c33_shift_count_imm(uint16_t word)
{
    unsigned count = extract32(word, 4, 4);

    return (count & 0x8) ? 8 : count;
}

static void gen_class4_shift(DisasContext *ctx, uint16_t word)
{
    unsigned op1 = extract32(word, 10, 3);
    unsigned op2 = extract32(word, 8, 2);
    unsigned rd = word & 0xf;
    TCGv_i32 count = tcg_temp_new_i32();

    if (op2 == 0) {
        tcg_gen_movi_i32(count, s1c33_shift_count_imm(word));
    } else {
        gen_shift_count_reg(count, cpu_regs[extract32(word, 4, 4)]);
    }

    switch (op1) {
    case 2: /* srl */
        tcg_gen_shr_i32(cpu_regs[rd], cpu_regs[rd], count);
        break;
    case 3: /* sll */
    case 5: /* sla */
        tcg_gen_shl_i32(cpu_regs[rd], cpu_regs[rd], count);
        break;
    case 4: /* sra */
        tcg_gen_sar_i32(cpu_regs[rd], cpu_regs[rd], count);
        break;
    case 6: /* rr */
        tcg_gen_rotr_i32(cpu_regs[rd], cpu_regs[rd], count);
        break;
    case 7: /* rl */
        tcg_gen_rotl_i32(cpu_regs[rd], cpu_regs[rd], count);
        break;
    default:
        g_assert_not_reached();
    }

    gen_set_nz(cpu_regs[rd]);
    gen_clear_ext(ctx);
}

static void gen_class4_swap(DisasContext *ctx, uint16_t word)
{
    unsigned op1 = extract32(word, 10, 3);
    unsigned rs = extract32(word, 4, 4);
    unsigned rd = word & 0xf;

    if (op1 == 4) {
        tcg_gen_bswap32_i32(cpu_regs[rd], cpu_regs[rs]);
    } else {
        TCGv_i32 high = tcg_temp_new_i32();
        TCGv_i32 low = tcg_temp_new_i32();

        tcg_gen_shli_i32(high, cpu_regs[rs], 8);
        tcg_gen_andi_i32(high, high, 0xff00ff00);
        tcg_gen_shri_i32(low, cpu_regs[rs], 8);
        tcg_gen_andi_i32(low, low, 0x00ff00ff);
        tcg_gen_or_i32(cpu_regs[rd], high, low);
    }
    gen_clear_ext(ctx);
}

static void gen_class4_scan(DisasContext *ctx, uint16_t word)
{
    unsigned op1 = extract32(word, 10, 3);
    unsigned rs = extract32(word, 4, 4);
    unsigned rd = word & 0xf;
    TCGv_i32 value = tcg_temp_new_i32();
    TCGv_i32 z = tcg_temp_new_i32();
    TCGv_i32 c = tcg_temp_new_i32();

    tcg_gen_shri_i32(value, cpu_regs[rs], 24);
    if (op1 == 2) {
        tcg_gen_xori_i32(value, value, 0xff);
    }
    tcg_gen_clzi_i32(value, value, 32);
    tcg_gen_subi_i32(value, value, 24);

    tcg_gen_mov_i32(cpu_regs[rd], value);
    tcg_gen_setcondi_i32(TCG_COND_EQ, z, value, 0);
    tcg_gen_setcondi_i32(TCG_COND_EQ, c, value, 8);
    tcg_gen_andi_i32(cpu_psr, cpu_psr,
                     ~(S1C33_PSR_N | S1C33_PSR_Z |
                       S1C33_PSR_V | S1C33_PSR_C));
    tcg_gen_shli_i32(z, z, 1);
    tcg_gen_or_i32(cpu_psr, cpu_psr, z);
    tcg_gen_shli_i32(c, c, 3);
    tcg_gen_or_i32(cpu_psr, cpu_psr, c);
    gen_clear_ext(ctx);
}

static void gen_class4_div(DisasContext *ctx, uint16_t word)
{
    unsigned op1 = extract32(word, 10, 3);
    unsigned rs = extract32(word, 4, 4);

    gen_helper_s1c33_div(tcg_env, tcg_constant_i32(op1 - 2),
                         cpu_regs[rs]);
    gen_clear_ext(ctx);
}

static void gen_ld_imm(DisasContext *ctx, uint16_t word)
{
    unsigned rd = word & 0xf;
    TCGv_i32 value = tcg_temp_new_i32();

    gen_extended_sign6(value, word);
    tcg_gen_mov_i32(cpu_regs[rd], value);
    gen_clear_ext(ctx);
}

static void gen_and_imm(DisasContext *ctx, uint16_t word)
{
    unsigned rd = word & 0xf;
    TCGv_i32 value = tcg_temp_new_i32();

    gen_extended_sign6(value, word);
    tcg_gen_and_i32(cpu_regs[rd], cpu_regs[rd], value);
    gen_set_nz(cpu_regs[rd]);
    gen_clear_ext(ctx);
}

static void gen_or_imm(DisasContext *ctx, uint16_t word)
{
    unsigned rd = word & 0xf;
    TCGv_i32 value = tcg_temp_new_i32();

    gen_extended_sign6(value, word);
    tcg_gen_or_i32(cpu_regs[rd], cpu_regs[rd], value);
    gen_set_nz(cpu_regs[rd]);
    gen_clear_ext(ctx);
}

static void gen_xor_imm(DisasContext *ctx, uint16_t word)
{
    unsigned rd = word & 0xf;
    TCGv_i32 value = tcg_temp_new_i32();

    gen_extended_sign6(value, word);
    tcg_gen_xor_i32(cpu_regs[rd], cpu_regs[rd], value);
    gen_set_nz(cpu_regs[rd]);
    gen_clear_ext(ctx);
}

static void gen_not_imm(DisasContext *ctx, uint16_t word)
{
    unsigned rd = word & 0xf;
    TCGv_i32 value = tcg_temp_new_i32();

    gen_extended_sign6(value, word);
    tcg_gen_not_i32(cpu_regs[rd], value);
    gen_set_nz(cpu_regs[rd]);
    gen_clear_ext(ctx);
}

static void gen_class1_memory(DisasContext *ctx, uint16_t word)
{
    unsigned op1 = extract32(word, 10, 3);
    unsigned postinc = extract32(word, 8, 1);
    unsigned rb = extract32(word, 4, 4);
    unsigned rd_rs = extract32(word, 0, 4);
    TCGv_i32 addr = tcg_temp_new_i32();
    MemOp mop;
    int size;

    (void)ctx;
    if (postinc) {
        tcg_gen_mov_i32(addr, cpu_regs[rb]);
    } else {
        gen_extended_addr(addr, cpu_regs[rb]);
    }

    switch (op1) {
    case 0:
        tcg_gen_qemu_ld_i32(cpu_regs[rd_rs], addr, 0, MO_SB);
        size = 1;
        gen_trace_mem(ctx, false, addr, cpu_regs[rd_rs], size);
        break;
    case 1:
        tcg_gen_qemu_ld_i32(cpu_regs[rd_rs], addr, 0, MO_UB);
        size = 1;
        gen_trace_mem(ctx, false, addr, cpu_regs[rd_rs], size);
        break;
    case 2:
        tcg_gen_qemu_ld_i32(cpu_regs[rd_rs], addr, 0, MO_LESW);
        size = 2;
        gen_trace_mem(ctx, false, addr, cpu_regs[rd_rs], size);
        break;
    case 3:
        tcg_gen_qemu_ld_i32(cpu_regs[rd_rs], addr, 0, MO_LEUW);
        size = 2;
        gen_trace_mem(ctx, false, addr, cpu_regs[rd_rs], size);
        break;
    case 4:
        tcg_gen_qemu_ld_i32(cpu_regs[rd_rs], addr, 0, MO_LEUL);
        size = 4;
        gen_trace_mem(ctx, false, addr, cpu_regs[rd_rs], size);
        break;
    case 5:
    case 6:
    case 7:
        mop = op1 == 5 ? MO_UB : op1 == 6 ? MO_LEUW : MO_LEUL;
        size = op1 == 5 ? 1 : op1 == 6 ? 2 : 4;
        tcg_gen_qemu_st_i32(cpu_regs[rd_rs], addr, 0, mop);
        gen_trace_mem(ctx, true, addr, cpu_regs[rd_rs], size);
        break;
    default:
        g_assert_not_reached();
    }

    if (postinc) {
        tcg_gen_addi_i32(cpu_regs[rb], cpu_regs[rb], size);
    }
    gen_clear_ext(ctx);
}

static void gen_class1_register(DisasContext *ctx, uint16_t word)
{
    unsigned op1 = extract32(word, 10, 3);
    unsigned rs = extract32(word, 4, 4);
    unsigned rd = extract32(word, 0, 4);
    TCGLabel *standard = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv_i32 ext_value = tcg_temp_new_i32();
    TCGv_i32 old = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();

    tcg_gen_brcondi_i32(TCG_COND_EQ, cpu_ext_count, 0, standard);
    gen_extended_imm13_26(ext_value);
    switch (op1) {
    case 0:
        tcg_gen_add_i32(cpu_regs[rd], cpu_regs[rs], ext_value);
        gen_set_add_flags(cpu_regs[rs], ext_value, cpu_regs[rd]);
        break;
    case 1:
        tcg_gen_sub_i32(cpu_regs[rd], cpu_regs[rs], ext_value);
        gen_set_sub_flags(cpu_regs[rs], ext_value, cpu_regs[rd]);
        break;
    case 2:
        tcg_gen_sub_i32(result, cpu_regs[rs], ext_value);
        gen_set_sub_flags(cpu_regs[rs], ext_value, result);
        break;
    case 3:
        tcg_gen_mov_i32(cpu_regs[rd], cpu_regs[rs]);
        break;
    case 4:
        tcg_gen_and_i32(cpu_regs[rd], cpu_regs[rs], ext_value);
        gen_set_nz(cpu_regs[rd]);
        break;
    case 5:
        tcg_gen_or_i32(cpu_regs[rd], cpu_regs[rs], ext_value);
        gen_set_nz(cpu_regs[rd]);
        break;
    case 6:
        tcg_gen_xor_i32(cpu_regs[rd], cpu_regs[rs], ext_value);
        gen_set_nz(cpu_regs[rd]);
        break;
    case 7:
        tcg_gen_not_i32(cpu_regs[rd], ext_value);
        gen_set_nz(cpu_regs[rd]);
        break;
    default:
        g_assert_not_reached();
    }
    tcg_gen_br(done);

    gen_set_label(standard);
    switch (op1) {
    case 0:
        tcg_gen_mov_i32(old, cpu_regs[rd]);
        tcg_gen_add_i32(cpu_regs[rd], cpu_regs[rd], cpu_regs[rs]);
        gen_set_add_flags(old, cpu_regs[rs], cpu_regs[rd]);
        break;
    case 1:
        tcg_gen_mov_i32(old, cpu_regs[rd]);
        tcg_gen_sub_i32(cpu_regs[rd], cpu_regs[rd], cpu_regs[rs]);
        gen_set_sub_flags(old, cpu_regs[rs], cpu_regs[rd]);
        break;
    case 2:
        tcg_gen_sub_i32(result, cpu_regs[rd], cpu_regs[rs]);
        gen_set_sub_flags(cpu_regs[rd], cpu_regs[rs], result);
        break;
    case 3:
        tcg_gen_mov_i32(cpu_regs[rd], cpu_regs[rs]);
        break;
    case 4:
        tcg_gen_and_i32(cpu_regs[rd], cpu_regs[rd], cpu_regs[rs]);
        gen_set_nz(cpu_regs[rd]);
        break;
    case 5:
        tcg_gen_or_i32(cpu_regs[rd], cpu_regs[rd], cpu_regs[rs]);
        gen_set_nz(cpu_regs[rd]);
        break;
    case 6:
        tcg_gen_xor_i32(cpu_regs[rd], cpu_regs[rd], cpu_regs[rs]);
        gen_set_nz(cpu_regs[rd]);
        break;
    case 7:
        tcg_gen_not_i32(cpu_regs[rd], cpu_regs[rs]);
        gen_set_nz(cpu_regs[rd]);
        break;
    default:
        g_assert_not_reached();
    }

    gen_set_label(done);
    gen_clear_ext(ctx);
}

static void gen_class2_sp_addr(DisasContext *ctx, TCGv_i32 addr,
                               uint16_t word, int size)
{
    uint32_t imm6 = extract32(word, 4, 6);
    TCGv_i32 tmp = tcg_temp_new_i32();

    switch (ctx->ext_count) {
    case 0:
        tcg_gen_addi_i32(addr, cpu_sp, imm6 * size);
        break;
    case 1:
        tcg_gen_shli_i32(addr, cpu_ext[0], 6);
        tcg_gen_ori_i32(addr, addr, imm6);
        tcg_gen_add_i32(addr, addr, cpu_sp);
        break;
    default:
        tcg_gen_shli_i32(addr, cpu_ext[0], 19);
        tcg_gen_shli_i32(tmp, cpu_ext[1], 6);
        tcg_gen_or_i32(addr, addr, tmp);
        tcg_gen_ori_i32(addr, addr, imm6);
        tcg_gen_add_i32(addr, addr, cpu_sp);
        break;
    }
}

static void gen_class2_stack_memory(DisasContext *ctx, uint16_t word)
{
    unsigned op1 = extract32(word, 10, 3);
    unsigned rd_rs = word & 0xf;
    TCGv_i32 addr = tcg_temp_new_i32();
    MemOp mop;
    int size;

    switch (op1) {
    case 0:
    case 1:
    case 5:
        size = 1;
        break;
    case 2:
    case 3:
    case 6:
        size = 2;
        break;
    case 4:
    case 7:
        size = 4;
        break;
    default:
        g_assert_not_reached();
    }

    gen_class2_sp_addr(ctx, addr, word, size);

    switch (op1) {
    case 0:
        tcg_gen_qemu_ld_i32(cpu_regs[rd_rs], addr, 0, MO_SB);
        gen_trace_mem(ctx, false, addr, cpu_regs[rd_rs], size);
        break;
    case 1:
        tcg_gen_qemu_ld_i32(cpu_regs[rd_rs], addr, 0, MO_UB);
        gen_trace_mem(ctx, false, addr, cpu_regs[rd_rs], size);
        break;
    case 2:
        tcg_gen_qemu_ld_i32(cpu_regs[rd_rs], addr, 0, MO_LESW);
        gen_trace_mem(ctx, false, addr, cpu_regs[rd_rs], size);
        break;
    case 3:
        tcg_gen_qemu_ld_i32(cpu_regs[rd_rs], addr, 0, MO_LEUW);
        gen_trace_mem(ctx, false, addr, cpu_regs[rd_rs], size);
        break;
    case 4:
        tcg_gen_qemu_ld_i32(cpu_regs[rd_rs], addr, 0, MO_LEUL);
        gen_trace_mem(ctx, false, addr, cpu_regs[rd_rs], size);
        break;
    case 5:
    case 6:
    case 7:
        mop = op1 == 5 ? MO_UB : op1 == 6 ? MO_LEUW : MO_LEUL;
        tcg_gen_qemu_st_i32(cpu_regs[rd_rs], addr, 0, mop);
        gen_trace_mem(ctx, true, addr, cpu_regs[rd_rs], size);
        break;
    default:
        g_assert_not_reached();
    }

    gen_clear_ext(ctx);
}

static void gen_bitop(DisasContext *ctx, uint16_t word)
{
    unsigned op1 = extract32(word, 10, 3);
    unsigned rb = extract32(word, 4, 4);
    unsigned bit = extract32(word, 0, 3);
    uint32_t mask = 1u << bit;
    TCGv_i32 addr = tcg_temp_new_i32();
    TCGv_i32 value = tcg_temp_new_i32();
    TCGv_i32 test = tcg_temp_new_i32();

    gen_extended_addr(addr, cpu_regs[rb]);
    tcg_gen_qemu_ld_i32(value, addr, 0, MO_UB);
    gen_trace_mem(ctx, false, addr, value, 1);

    switch (op1) {
    case 2: /* btst */
        tcg_gen_andi_i32(test, value, mask);
        tcg_gen_setcondi_i32(TCG_COND_EQ, test, test, 0);
        tcg_gen_andi_i32(cpu_psr, cpu_psr, ~(1u << 1));
        tcg_gen_shli_i32(test, test, 1);
        tcg_gen_or_i32(cpu_psr, cpu_psr, test);
        break;
    case 3: /* bclr */
        tcg_gen_andi_i32(value, value, ~mask);
        tcg_gen_qemu_st_i32(value, addr, 0, MO_UB);
        gen_trace_mem(ctx, true, addr, value, 1);
        break;
    case 4: /* bset */
        tcg_gen_ori_i32(value, value, mask);
        tcg_gen_qemu_st_i32(value, addr, 0, MO_UB);
        gen_trace_mem(ctx, true, addr, value, 1);
        break;
    case 5: /* bnot */
        tcg_gen_xori_i32(value, value, mask);
        tcg_gen_qemu_st_i32(value, addr, 0, MO_UB);
        gen_trace_mem(ctx, true, addr, value, 1);
        break;
    default:
        g_assert_not_reached();
    }

    gen_clear_ext(ctx);
}

static void gen_class5_reg_transfer(DisasContext *ctx, uint16_t word)
{
    unsigned op1 = extract32(word, 10, 3);
    unsigned rs = extract32(word, 4, 4);
    unsigned rd = extract32(word, 0, 4);

    switch (op1) {
    case 0:
        tcg_gen_ext8s_i32(cpu_regs[rd], cpu_regs[rs]);
        break;
    case 1:
        tcg_gen_ext8u_i32(cpu_regs[rd], cpu_regs[rs]);
        break;
    case 2:
        tcg_gen_ext16s_i32(cpu_regs[rd], cpu_regs[rs]);
        break;
    case 3:
        tcg_gen_ext16u_i32(cpu_regs[rd], cpu_regs[rs]);
        break;
    default:
        g_assert_not_reached();
    }

    gen_clear_ext(ctx);
}

static void gen_class5_multiply(DisasContext *ctx, uint16_t word)
{
    unsigned op1 = extract32(word, 10, 3);
    unsigned rs = extract32(word, 4, 4);
    unsigned rd = extract32(word, 0, 4);

    gen_helper_s1c33_mul(tcg_env, tcg_constant_i32(op1),
                         cpu_regs[rd], cpu_regs[rs]);
    gen_clear_ext(ctx);
}

static void gen_adc_sbc(DisasContext *ctx, uint16_t word, bool subtract)
{
    unsigned rs = extract32(word, 4, 4);
    unsigned rd = extract32(word, 0, 4);
    TCGv_i32 lhs = tcg_temp_new_i32();
    TCGv_i32 rhs = tcg_temp_new_i32();
    TCGv_i32 carry = tcg_temp_new_i32();
    TCGv_i32 operand = tcg_temp_new_i32();

    tcg_gen_mov_i32(lhs, cpu_regs[rd]);
    tcg_gen_mov_i32(rhs, cpu_regs[rs]);
    tcg_gen_shri_i32(carry, cpu_psr, 3);
    tcg_gen_andi_i32(carry, carry, 1);
    tcg_gen_add_i32(operand, rhs, carry);
    if (subtract) {
        tcg_gen_sub_i32(cpu_regs[rd], lhs, operand);
        gen_set_sbc_flags(lhs, rhs, carry, cpu_regs[rd]);
    } else {
        tcg_gen_add_i32(cpu_regs[rd], lhs, operand);
        gen_set_adc_flags(lhs, rhs, carry, cpu_regs[rd]);
    }
    gen_clear_ext(ctx);
}

static void gen_add_imm(DisasContext *ctx, uint16_t word)
{
    unsigned rd = word & 0xf;
    TCGv_i32 value = tcg_temp_new_i32();
    TCGv_i32 old = tcg_temp_new_i32();

    gen_extended_imm6(value, word);
    tcg_gen_mov_i32(old, cpu_regs[rd]);
    tcg_gen_add_i32(cpu_regs[rd], cpu_regs[rd], value);
    gen_set_add_flags(old, value, cpu_regs[rd]);
    gen_clear_ext(ctx);
}

static void gen_sub_imm(DisasContext *ctx, uint16_t word)
{
    unsigned rd = word & 0xf;
    TCGv_i32 value = tcg_temp_new_i32();
    TCGv_i32 old = tcg_temp_new_i32();

    gen_extended_imm6(value, word);
    tcg_gen_mov_i32(old, cpu_regs[rd]);
    tcg_gen_sub_i32(cpu_regs[rd], cpu_regs[rd], value);
    gen_set_sub_flags(old, value, cpu_regs[rd]);
    gen_clear_ext(ctx);
}

static void gen_cmp_imm(DisasContext *ctx, uint16_t word)
{
    unsigned rd = word & 0xf;
    TCGv_i32 value = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();

    gen_extended_sign6(value, word);
    tcg_gen_sub_i32(result, cpu_regs[rd], value);
    gen_set_sub_flags(cpu_regs[rd], value, result);
    gen_clear_ext(ctx);
}

static void translate_one(DisasContext *ctx, uint16_t word)
{
    if ((word & 0xe000) == 0xc000) {
        gen_ext(ctx, word);
        return;
    }

    switch (word) {
    case 0x0000:
        gen_clear_ext(ctx);
        return;
    case 0x0400:
        tcg_gen_movi_i32(cpu_pc, ctx->base.pc_next);
        gen_helper_s1c33_brk(tcg_env, tcg_constant_i32(ctx->pc));
        ctx->base.is_jmp = DISAS_NORETURN;
        return;
    case 0x0040:
        tcg_gen_movi_i32(cpu_pc, ctx->base.pc_next);
        gen_helper_s1c33_sleep(tcg_env, tcg_constant_i32(ctx->pc));
        ctx->base.is_jmp = DISAS_NORETURN;
        return;
    case 0x0080:
        tcg_gen_movi_i32(cpu_pc, ctx->base.pc_next);
        gen_helper_s1c33_halt(tcg_env, tcg_constant_i32(ctx->pc));
        ctx->base.is_jmp = DISAS_NORETURN;
        return;
    case 0x04c0:
        gen_reti(ctx);
        return;
    default:
        break;
    }

    if (word >= 0x0200 && word <= 0x020f) {
        gen_pushn(ctx, word);
        return;
    }
    if (word >= 0x0240 && word <= 0x024f) {
        gen_popn(ctx, word);
        return;
    }
    if (word == 0x0640 || word == 0x0740) {
        gen_ret(ctx, word);
        return;
    }
    if ((word >= 0x0600 && word <= 0x060f) ||
        (word >= 0x0700 && word <= 0x070f)) {
        gen_register_call_or_jump(ctx, word, true);
        return;
    }
    if ((word >= 0x0680 && word <= 0x068f) ||
        (word >= 0x0780 && word <= 0x078f)) {
        gen_register_call_or_jump(ctx, word, false);
        return;
    }
    if (word >= 0x0800 && word <= 0x1bff &&
        extract32(word, 9, 4) >= 4 && extract32(word, 9, 4) <= 13) {
        gen_cond_branch(ctx, word);
        return;
    }
    if (word >= 0x1c00 && word <= 0x1dff) {
        gen_relative_call_or_jump(ctx, word, true);
        return;
    }
    if (word >= 0x1e00 && word <= 0x1fff) {
        gen_relative_call_or_jump(ctx, word, false);
        return;
    }
    if (word >= 0x2000 && word <= 0x3dff &&
        (extract32(word, 8, 2) == 0 || extract32(word, 8, 2) == 1)) {
        gen_class1_memory(ctx, word);
        return;
    }
    if (word >= 0x2200 && word <= 0x3eff && extract32(word, 8, 2) == 2) {
        gen_class1_register(ctx, word);
        return;
    }
    if (word >= 0x4000 && word <= 0x5fff) {
        gen_class2_stack_memory(ctx, word);
        return;
    }
    if ((word >= 0xa800 && word <= 0xb4f7) &&
        extract32(word, 8, 2) == 0 &&
        extract32(word, 10, 3) >= 2 &&
        extract32(word, 10, 3) <= 5 &&
        (word & 0x8) == 0) {
        gen_bitop(ctx, word);
        return;
    }
    if (word >= 0xa100 && word <= 0xadff &&
        extract32(word, 8, 2) == 1 &&
        extract32(word, 10, 3) <= 3) {
        gen_class5_reg_transfer(ctx, word);
        return;
    }
    if (word >= 0xa200 && word <= 0xaeff &&
        extract32(word, 8, 2) == 2 &&
        extract32(word, 10, 3) <= 3) {
        gen_class5_multiply(ctx, word);
        return;
    }
    if (word >= 0xbf40 && word <= 0xbf5f) {
        gen_psr_bit(ctx, word, true);
        gen_clear_ext(ctx);
        return;
    }
    if (word >= 0xbf60 && word <= 0xbf7f) {
        gen_psr_bit(ctx, word, false);
        gen_clear_ext(ctx);
        return;
    }
    if (word >= 0x6000 && word <= 0x63ff) {
        gen_add_imm(ctx, word);
        return;
    }
    if (word >= 0x6400 && word <= 0x67ff) {
        gen_sub_imm(ctx, word);
        return;
    }
    if (word >= 0x8000 && word <= 0x83ff) {
        gen_sp_adjust(ctx, word, false);
        return;
    }
    if (word >= 0x8400 && word <= 0x87ff) {
        gen_sp_adjust(ctx, word, true);
        return;
    }
    if (word >= 0x8800 && word <= 0x9dff &&
        extract32(word, 8, 2) <= 1) {
        gen_class4_shift(ctx, word);
        return;
    }
    if ((word & 0xff00) == 0x9200 || (word & 0xff00) == 0x9a00) {
        gen_class4_swap(ctx, word);
        return;
    }
    if (word >= 0x8a00 && word <= 0x8eff &&
        extract32(word, 8, 2) == 2 &&
        extract32(word, 10, 3) >= 2 &&
        extract32(word, 10, 3) <= 3) {
        gen_class4_scan(ctx, word);
        return;
    }
    if (word >= 0x8b00 && word <= 0x9bf0 &&
        extract32(word, 8, 2) == 3 &&
        extract32(word, 10, 3) >= 2 &&
        extract32(word, 10, 3) <= 6 &&
        (word & 0xf) == 0) {
        gen_class4_div(ctx, word);
        return;
    }
    if (word >= 0x6800 && word <= 0x6bff) {
        gen_cmp_imm(ctx, word);
        return;
    }
    if (word >= 0x6c00 && word <= 0x6fff) {
        gen_ld_imm(ctx, word);
        return;
    }
    if (word >= 0x7000 && word <= 0x73ff) {
        gen_and_imm(ctx, word);
        return;
    }
    if (word >= 0x7400 && word <= 0x77ff) {
        gen_or_imm(ctx, word);
        return;
    }
    if (word >= 0x7800 && word <= 0x7bff) {
        gen_xor_imm(ctx, word);
        return;
    }
    if (word >= 0x7c00 && word <= 0x7fff) {
        gen_not_imm(ctx, word);
        return;
    }
    if (word >= 0xa000 && word <= 0xa0f3 && (word & 0xc) == 0) {
        gen_ld_reg_to_special(ctx, word);
        return;
    }
    if (word >= 0xa400 && word <= 0xa43f) {
        gen_ld_special_to_reg(ctx, word);
        return;
    }
    if ((word & 0xff00) == 0xb800) {
        gen_adc_sbc(ctx, word, false);
        return;
    }
    if ((word & 0xff00) == 0xbc00) {
        gen_adc_sbc(ctx, word, true);
        return;
    }

    gen_unimplemented(ctx, word);
}

static void s1c33_tr_init_disas_context(DisasContextBase *db, CPUState *cs)
{
    DisasContext *ctx = container_of(db, DisasContext, base);

    ctx->env = cpu_env(cs);
    ctx->ext_count = MIN(ctx->env->ext_count, 2);
    ctx->ext[0] = ctx->env->ext[0];
    ctx->ext[1] = ctx->env->ext[1];
}

static void s1c33_tr_tb_start(DisasContextBase *db, CPUState *cs)
{
    gen_helper_s1c33_record_tb(tcg_env, tcg_constant_i32(db->pc_first));
}

static void s1c33_tr_insn_start(DisasContextBase *db, CPUState *cs)
{
    tcg_gen_insn_start(db->pc_next, 0, 0);
}

static void s1c33_tr_translate_insn(DisasContextBase *db, CPUState *cs)
{
    DisasContext *ctx = container_of(db, DisasContext, base);
    uint16_t word;

    ctx->pc = ctx->base.pc_next;
    word = translator_lduw_end(ctx->env, &ctx->base, ctx->base.pc_next,
                               MO_LE);
    ctx->base.pc_next += 2;
    translate_one(ctx, word);
}

static void s1c33_tr_tb_stop(DisasContextBase *db, CPUState *cs)
{
    DisasContext *ctx = container_of(db, DisasContext, base);

    switch (ctx->base.is_jmp) {
    case DISAS_NEXT:
    case DISAS_TOO_MANY:
        gen_goto_tb(ctx, 0, db->pc_next);
        break;
    case DISAS_NORETURN:
        break;
    default:
        g_assert_not_reached();
    }
}

static const TranslatorOps s1c33_tr_ops = {
    .init_disas_context = s1c33_tr_init_disas_context,
    .tb_start = s1c33_tr_tb_start,
    .insn_start = s1c33_tr_insn_start,
    .translate_insn = s1c33_tr_translate_insn,
    .tb_stop = s1c33_tr_tb_stop,
};

void s1c33_translate_code(CPUState *cs, TranslationBlock *tb,
                          int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext dc = { };

    translator_loop(cs, tb, max_insns, pc, host_pc, &s1c33_tr_ops, &dc.base);
}

void s1c33_translate_init(void)
{
    static const char * const names[S1C33_NUM_REGS] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    };
    int i;

    for (i = 0; i < S1C33_NUM_REGS; i++) {
        cpu_regs[i] = tcg_global_mem_new_i32(tcg_env,
                                             offsetof(CPUS1C33State, regs[i]),
                                             names[i]);
    }
    cpu_pc = tcg_global_mem_new_i32(tcg_env, offsetof(CPUS1C33State, pc),
                                    "pc");
    cpu_psr = tcg_global_mem_new_i32(tcg_env, offsetof(CPUS1C33State, psr),
                                     "psr");
    cpu_sp = tcg_global_mem_new_i32(tcg_env, offsetof(CPUS1C33State, sp),
                                    "sp");
    cpu_alr = tcg_global_mem_new_i32(tcg_env, offsetof(CPUS1C33State, alr),
                                     "alr");
    cpu_ahr = tcg_global_mem_new_i32(tcg_env, offsetof(CPUS1C33State, ahr),
                                     "ahr");
    cpu_ext[0] = tcg_global_mem_new_i32(tcg_env,
                                        offsetof(CPUS1C33State, ext[0]),
                                        "ext0");
    cpu_ext[1] = tcg_global_mem_new_i32(tcg_env,
                                        offsetof(CPUS1C33State, ext[1]),
                                        "ext1");
    cpu_ext_count = tcg_global_mem_new_i32(tcg_env,
                                           offsetof(CPUS1C33State, ext_count),
                                           "ext_count");
}
