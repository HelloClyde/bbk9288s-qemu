/*
 * Epson S1C33 GDB stub.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "gdbstub/helpers.h"

int s1c33_cpu_gdb_read_register(CPUState *cs, GByteArray *buf, int n)
{
    CPUS1C33State *env = cpu_env(cs);

    if (n >= 0 && n < S1C33_NUM_REGS) {
        return gdb_get_reg32(buf, env->regs[n]);
    }
    if (n == S1C33_NUM_REGS) {
        return gdb_get_reg32(buf, env->pc);
    }
    if (n == S1C33_NUM_REGS + 1) {
        return gdb_get_reg32(buf, env->psr);
    }
    if (n == S1C33_NUM_REGS + 2) {
        return gdb_get_reg32(buf, env->sp);
    }
    if (n == S1C33_NUM_REGS + 3) {
        return gdb_get_reg32(buf, env->alr);
    }
    if (n == S1C33_NUM_REGS + 4) {
        return gdb_get_reg32(buf, env->ahr);
    }
    return 0;
}

int s1c33_cpu_gdb_write_register(CPUState *cs, uint8_t *buf, int n)
{
    CPUS1C33State *env = cpu_env(cs);

    if (n >= 0 && n < S1C33_NUM_REGS) {
        env->regs[n] = ldl_le_p(buf);
        return 4;
    }
    if (n == S1C33_NUM_REGS) {
        env->pc = ldl_le_p(buf);
        return 4;
    }
    if (n == S1C33_NUM_REGS + 1) {
        env->psr = ldl_le_p(buf);
        return 4;
    }
    if (n == S1C33_NUM_REGS + 2) {
        env->sp = ldl_le_p(buf);
        return 4;
    }
    if (n == S1C33_NUM_REGS + 3) {
        env->alr = ldl_le_p(buf);
        return 4;
    }
    if (n == S1C33_NUM_REGS + 4) {
        env->ahr = ldl_le_p(buf);
        return 4;
    }
    return 0;
}
