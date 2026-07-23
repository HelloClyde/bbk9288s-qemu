/*
 * Epson S1C33 helper functions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/log.h"
#include "system/runstate.h"

#define S1C33_DEBUG_LOW_RAM_LIMIT   0x00040000u
#define S1C33_DEBUG_SDRAM_BASE      0x02000000u
#define S1C33_DEBUG_SDRAM_LIMIT     0x04000000u

void helper_s1c33_record_tb(CPUS1C33State *env, uint32_t pc)
{
    CPUState *cs = env_cpu(env);
    S1C33CPU *cpu = S1C33_CPU(cs);

    env->recent_tbs[env->recent_tb_pos] = pc;
    env->recent_tb_pos = (env->recent_tb_pos + 1) % S1C33_RECENT_TB_LEN;
    if (env->recent_tb_count < S1C33_RECENT_TB_LEN) {
        env->recent_tb_count++;
    }

    if (cpu->trace_exec) {
        uint32_t end = cpu->trace_exec_end != 0 ?
                       cpu->trace_exec_end : UINT32_MAX;

        if ((cpu->trace_exec_start == 0 && cpu->trace_exec_end == 0) ||
            (pc >= cpu->trace_exec_start && pc <= end)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "s1c33-exec: tb pc=0x%08x sp=0x%08x "
                          "psr=0x%08x\n",
                          pc, env->sp, env->psr);
        }
    }
}

void helper_s1c33_write_psr(CPUS1C33State *env, uint32_t value, uint32_t pc,
                            uint32_t op)
{
    CPUState *cs = env_cpu(env);
    S1C33CPU *cpu = S1C33_CPU(cs);
    uint32_t old = env->psr;
    uint32_t watched = S1C33_PSR_IE | S1C33_PSR_IL_MASK;

    env->psr = value;
    if (cpu->trace_psr && ((old ^ value) & watched) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s1c33-psr: pc=0x%08x op=0x%04x "
                      "old=0x%08x new=0x%08x ie=%u->%u il=%u->%u\n",
                      pc, op & 0xffff, old, value,
                      (old & S1C33_PSR_IE) != 0,
                      (value & S1C33_PSR_IE) != 0,
                      (old & S1C33_PSR_IL_MASK) >> S1C33_PSR_IL_SHIFT,
                      (value & S1C33_PSR_IL_MASK) >> S1C33_PSR_IL_SHIFT);
    }
}

enum {
    S1C33_DIV0S,
    S1C33_DIV0U,
    S1C33_DIV1,
    S1C33_DIV2S,
    S1C33_DIV3S,
};

enum {
    S1C33_MLT_H,
    S1C33_MLTU_H,
    S1C33_MLT_W,
    S1C33_MLTU_W,
};

enum {
    S1C33_TRACE_CALL,
    S1C33_TRACE_REG_CALL,
    S1C33_TRACE_RET,
    S1C33_TRACE_RETI,
};

static bool s1c33_trace_call_addr_match(S1C33CPU *cpu, uint32_t addr)
{
    uint32_t end;

    if (cpu->trace_calls_start == 0 && cpu->trace_calls_end == 0) {
        return true;
    }

    end = cpu->trace_calls_end != 0 ? cpu->trace_calls_end : UINT32_MAX;
    return addr >= cpu->trace_calls_start && addr <= end;
}

void helper_s1c33_trace_branch(CPUS1C33State *env, uint32_t kind,
                               uint32_t from, uint32_t to, uint32_t aux)
{
    CPUState *cs = env_cpu(env);
    S1C33CPU *cpu = S1C33_CPU(cs);
    const char *name;

    if (!cpu->trace_calls) {
        return;
    }
    if (!s1c33_trace_call_addr_match(cpu, from) &&
        !s1c33_trace_call_addr_match(cpu, to)) {
        return;
    }

    switch (kind) {
    case S1C33_TRACE_CALL:
        name = "call";
        break;
    case S1C33_TRACE_REG_CALL:
        name = "call-reg";
        break;
    case S1C33_TRACE_RET:
        name = "ret";
        break;
    case S1C33_TRACE_RETI:
        name = "reti";
        break;
    default:
        name = "branch";
        break;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "s1c33-call: %-8s from=0x%08x to=0x%08x "
                  "aux=0x%08x sp=0x%08x psr=0x%08x "
                  "r6=0x%08x r7=0x%08x r8=0x%08x r9=0x%08x\n",
                  name, from, to, aux, env->sp, env->psr,
                  env->regs[6], env->regs[7], env->regs[8], env->regs[9]);
}

void helper_s1c33_trace_mem(CPUS1C33State *env, uint32_t is_write,
                            uint32_t pc, uint32_t addr, uint32_t value,
                            uint32_t size)
{
    CPUState *cs = env_cpu(env);
    S1C33CPU *cpu = S1C33_CPU(cs);
    uint64_t access_last;
    uint32_t mask;

    if (!cpu->trace_mem) {
        return;
    }

    if (cpu->trace_mem_pc_start != 0 || cpu->trace_mem_pc_end != 0) {
        uint32_t pc_end = cpu->trace_mem_pc_end != 0 ?
                          cpu->trace_mem_pc_end : UINT32_MAX;

        if (pc < cpu->trace_mem_pc_start || pc > pc_end) {
            return;
        }
    }

    if (cpu->trace_mem_start != 0 || cpu->trace_mem_end != 0) {
        uint32_t end = cpu->trace_mem_end != 0 ?
                       cpu->trace_mem_end : UINT32_MAX;

        access_last = (uint64_t)addr + size - 1;
        if (addr > end || access_last < cpu->trace_mem_start) {
            return;
        }
    }

    mask = size >= 4 ? UINT32_MAX : (1u << (size * 8)) - 1;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "s1c33-mem: %c%u pc=0x%08x addr=0x%08x value=0x%0*x\n",
                  is_write ? 'W' : 'R', size, pc, addr,
                  2 * size, value & mask);
}

static bool s1c33_psr_ds(CPUS1C33State *env)
{
    return (env->psr & S1C33_PSR_DS) != 0;
}

static bool s1c33_psr_n(CPUS1C33State *env)
{
    return (env->psr & S1C33_PSR_N) != 0;
}

static uint64_t s1c33_div_ext33(uint32_t value, bool sign)
{
    return (uint64_t)value | ((uint64_t)sign << 32);
}

void helper_s1c33_div(CPUS1C33State *env, uint32_t op, uint32_t rs)
{
    bool ds;
    bool n;
    uint64_t pair;
    uint64_t lhs;
    uint64_t rhs;
    uint64_t tmp;
    uint32_t tmp32;

    switch (op) {
    case S1C33_DIV0S:
        env->ahr = (env->alr & 0x80000000u) ? UINT32_MAX : 0;
        env->psr &= ~(S1C33_PSR_DS | S1C33_PSR_N);
        if (env->alr & 0x80000000u) {
            env->psr |= S1C33_PSR_DS;
        }
        if (rs & 0x80000000u) {
            env->psr |= S1C33_PSR_N;
        }
        break;

    case S1C33_DIV0U:
        env->ahr = 0;
        env->psr &= ~(S1C33_PSR_DS | S1C33_PSR_N);
        break;

    case S1C33_DIV1:
        pair = (((uint64_t)env->ahr << 32) | env->alr) << 1;
        env->ahr = pair >> 32;
        env->alr = pair;
        ds = s1c33_psr_ds(env);
        n = s1c33_psr_n(env);
        lhs = s1c33_div_ext33(env->ahr, ds);
        rhs = s1c33_div_ext33(rs, n);
        if (!ds && !n) {
            tmp = (lhs - rhs) & 0x1ffffffffULL;
            if ((tmp >> 32) == 0) {
                env->ahr = tmp;
                env->alr |= 1;
            }
        } else if (ds && !n) {
            tmp = (lhs + rhs) & 0x1ffffffffULL;
            if ((tmp >> 32) != 0) {
                env->ahr = tmp;
                env->alr |= 1;
            }
        } else if (!ds && n) {
            tmp = (lhs + rhs) & 0x1ffffffffULL;
            if ((tmp >> 32) == 0) {
                env->ahr = tmp;
                env->alr |= 1;
            }
        } else {
            tmp = (lhs - rhs) & 0x1ffffffffULL;
            if ((tmp >> 32) != 0) {
                env->ahr = tmp;
                env->alr |= 1;
            }
        }
        break;

    case S1C33_DIV2S:
        if (s1c33_psr_ds(env)) {
            if (s1c33_psr_n(env)) {
                tmp32 = env->ahr - rs;
            } else {
                tmp32 = env->ahr + rs;
            }
            if (tmp32 == 0) {
                env->ahr = tmp32;
                env->alr++;
            }
        }
        break;

    case S1C33_DIV3S:
        if (s1c33_psr_ds(env) != s1c33_psr_n(env)) {
            env->alr = -env->alr;
        }
        break;

    default:
        g_assert_not_reached();
    }
}

void helper_s1c33_mul(CPUS1C33State *env, uint32_t op, uint32_t rd,
                      uint32_t rs)
{
    int64_t sresult;
    uint64_t uresult;

    switch (op) {
    case S1C33_MLT_H:
        sresult = (int32_t)(int16_t)rd * (int32_t)(int16_t)rs;
        env->alr = sresult;
        break;
    case S1C33_MLTU_H:
        env->alr = (uint32_t)(uint16_t)rd * (uint32_t)(uint16_t)rs;
        break;
    case S1C33_MLT_W:
        sresult = (int64_t)(int32_t)rd * (int64_t)(int32_t)rs;
        env->alr = sresult;
        env->ahr = (uint64_t)sresult >> 32;
        break;
    case S1C33_MLTU_W:
        uresult = (uint64_t)rd * (uint64_t)rs;
        env->alr = uresult;
        env->ahr = uresult >> 32;
        break;
    default:
        g_assert_not_reached();
    }
}

static void s1c33_log_recent_tbs(CPUS1C33State *env)
{
    uint32_t count = env->recent_tb_count;
    uint32_t start;
    uint32_t i;

    if (count == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "s1c33: recent tb: none\n");
        return;
    }

    start = count == S1C33_RECENT_TB_LEN ? env->recent_tb_pos : 0;
    qemu_log_mask(LOG_GUEST_ERROR, "s1c33: recent tb entries=%u\n", count);
    for (i = 0; i < count; i++) {
        uint32_t idx = (start + i) % S1C33_RECENT_TB_LEN;

        qemu_log_mask(LOG_GUEST_ERROR, "s1c33: recent tb[%02u]=0x%08x\n",
                      i, env->recent_tbs[idx]);
    }
}

static bool s1c33_read_debug(CPUS1C33State *env, uint32_t addr,
                             void *buf, size_t len)
{
    CPUState *cs = env_cpu(env);
    uint64_t end = (uint64_t)addr + len;

    if (!((end <= S1C33_DEBUG_LOW_RAM_LIMIT) ||
          (addr >= S1C33_DEBUG_SDRAM_BASE &&
           end <= S1C33_DEBUG_SDRAM_LIMIT))) {
        return false;
    }

    return cpu_memory_rw_debug(cs, addr, buf, len, false) == 0;
}

static bool s1c33_read_u32_debug(CPUS1C33State *env, uint32_t addr,
                                 uint32_t *value)
{
    uint8_t buf[4];

    if (!s1c33_read_debug(env, addr, buf, sizeof(buf))) {
        return false;
    }

    *value = ldl_le_p(buf);
    return true;
}

static bool s1c33_ascii_preview(CPUS1C33State *env, uint32_t addr,
                                char *out, size_t out_size)
{
    uint8_t buf[48];
    size_t i;
    size_t n = 0;

    if (out_size == 0 || !s1c33_read_debug(env, addr, buf, sizeof(buf))) {
        return false;
    }

    for (i = 0; i < sizeof(buf) && n + 1 < out_size; i++) {
        uint8_t ch = buf[i];

        if (ch == 0) {
            break;
        }
        if (ch < 0x20 || ch > 0x7e) {
            break;
        }
        out[n++] = ch;
    }
    out[n] = 0;

    return n >= 4;
}

static void s1c33_log_pointer_preview(CPUS1C33State *env, const char *name,
                                      uint32_t value)
{
    char text[49];

    if (s1c33_ascii_preview(env, value, text, sizeof(text))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s1c33: ptr %-7s=0x%08x ascii=\"%s\"\n",
                      name, value, text);
    }
}

static void s1c33_log_stack(CPUS1C33State *env)
{
    uint32_t i;

    qemu_log_mask(LOG_GUEST_ERROR, "s1c33: stack sp=0x%08x\n", env->sp);
    for (i = 0; i < 24; i++) {
        uint32_t addr = env->sp + i * 4;
        uint32_t value;
        char text[49];

        if (!s1c33_read_u32_debug(env, addr, &value)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "s1c33: stack[%02u] addr=0x%08x unreadable\n",
                          i, addr);
            break;
        }

        if (s1c33_ascii_preview(env, value, text, sizeof(text))) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "s1c33: stack[%02u] addr=0x%08x "
                          "value=0x%08x ascii=\"%s\"\n",
                          i, addr, value, text);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "s1c33: stack[%02u] addr=0x%08x value=0x%08x\n",
                          i, addr, value);
        }
    }
}

static void s1c33_log_cpu_state(CPUS1C33State *env)
{
    int i;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "s1c33: state pc=0x%08x psr=0x%08x sp=0x%08x "
                  "alr=0x%08x ahr=0x%08x ext_count=%u "
                  "ext0=0x%08x ext1=0x%08x ttbr=0x%08x "
                  "irq=%u vector=%u level=%u\n",
                  env->pc, env->psr, env->sp, env->alr, env->ahr,
                  env->ext_count, env->ext[0], env->ext[1], env->ttbr,
                  env->irq_pending, env->irq_vector, env->irq_level);
    for (i = 0; i < S1C33_NUM_REGS; i += 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s1c33: regs r%-2d=0x%08x r%-2d=0x%08x "
                      "r%-2d=0x%08x r%-2d=0x%08x\n",
                      i, env->regs[i], i + 1, env->regs[i + 1],
                      i + 2, env->regs[i + 2], i + 3,
                      env->regs[i + 3]);
    }
    for (i = 0; i < S1C33_NUM_REGS; i++) {
        char name[4];

        snprintf(name, sizeof(name), "r%d", i);
        s1c33_log_pointer_preview(env, name, env->regs[i]);
    }
    s1c33_log_pointer_preview(env, "pc", env->pc);
    s1c33_log_pointer_preview(env, "sp", env->sp);
    s1c33_log_pointer_preview(env, "alr", env->alr);
    s1c33_log_pointer_preview(env, "ahr", env->ahr);
    s1c33_log_stack(env);
    s1c33_log_recent_tbs(env);
}

static G_NORETURN void s1c33_enter_halt(CPUS1C33State *env, CPUState *cs,
                                        bool request_shutdown,
                                        bool is_sleep)
{
    cs->halted = 1;
    env->in_sleep = is_sleep;
    if (request_shutdown) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
    cs->exception_index = EXCP_HLT;
    /*
     * The translator sets env->pc to the architectural resume address before
     * calling the helper.  Restoring from the TCG opc would move HALT/SLP back
     * to the sleeping instruction, so interrupts would return to sleep again.
     */
    cpu_loop_exit(cs);
}

G_NORETURN void helper_s1c33_brk(CPUS1C33State *env, uint32_t insn_pc)
{
    CPUState *cs = env_cpu(env);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "s1c33: BRK at pc=0x%08x next_pc=0x%08x\n",
                  insn_pc, env->pc);
    s1c33_log_cpu_state(env);
    s1c33_enter_halt(env, cs, true, false);
}

G_NORETURN void helper_s1c33_unimplemented(CPUS1C33State *env,
                                           uint32_t insn)
{
    CPUState *cs = env_cpu(env);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "s1c33: unimplemented opcode 0x%04x at pc=0x%08x\n",
                  insn & 0xffff, env->pc);
    s1c33_log_cpu_state(env);
    s1c33_enter_halt(env, cs, true, false);
}

G_NORETURN void helper_s1c33_sleep(CPUS1C33State *env, uint32_t insn_pc)
{
    CPUState *cs = env_cpu(env);
    S1C33CPU *cpu = S1C33_CPU(cs);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "s1c33: SLP at pc=0x%08x next_pc=0x%08x\n",
                  insn_pc, env->pc);
    s1c33_log_cpu_state(env);
    s1c33_enter_halt(env, cs, cpu->exit_on_halt, true);
}

G_NORETURN void helper_s1c33_halt(CPUS1C33State *env, uint32_t insn_pc)
{
    CPUState *cs = env_cpu(env);
    S1C33CPU *cpu = S1C33_CPU(cs);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "s1c33: HALT at pc=0x%08x next_pc=0x%08x\n",
                  insn_pc, env->pc);
    s1c33_log_cpu_state(env);
    s1c33_enter_halt(env, cs, cpu->exit_on_halt, false);
}
