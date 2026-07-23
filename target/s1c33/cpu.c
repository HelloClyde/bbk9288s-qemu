/*
 * Epson S1C33 CPU.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/qemu-print.h"
#include "cpu.h"
#include "exec/cputlb.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "hw/core/qdev-properties.h"
#include "tcg/debug-assert.h"
#include "accel/tcg/cpu-ops.h"

static void s1c33_cpu_set_pc(CPUState *cs, vaddr value)
{
    S1C33CPU *cpu = S1C33_CPU(cs);

    cpu->env.pc = value;
}

static vaddr s1c33_cpu_get_pc(CPUState *cs)
{
    S1C33CPU *cpu = S1C33_CPU(cs);

    return cpu->env.pc;
}

static TCGTBCPUState s1c33_get_tb_cpu_state(CPUState *cs)
{
    CPUS1C33State *env = cpu_env(cs);

    return (TCGTBCPUState){ .pc = env->pc, .flags = 0 };
}

static uint8_t s1c33_cpu_interrupt_level(CPUS1C33State *env)
{
    return (env->psr & S1C33_PSR_IL_MASK) >> S1C33_PSR_IL_SHIFT;
}

static bool s1c33_cpu_irq_ready(CPUS1C33State *env)
{
    return env->irq_pending &&
           (env->psr & S1C33_PSR_IE) != 0 &&
           env->irq_level > s1c33_cpu_interrupt_level(env);
}

static void s1c33_cpu_synchronize_from_tb(CPUState *cs,
                                          const TranslationBlock *tb)
{
    S1C33CPU *cpu = S1C33_CPU(cs);

    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu->env.pc = tb->pc;
}

static void s1c33_restore_state_to_opc(CPUState *cs,
                                       const TranslationBlock *tb,
                                       const uint64_t *data)
{
    S1C33CPU *cpu = S1C33_CPU(cs);

    cpu->env.pc = data[0];
}

static bool s1c33_cpu_has_work(CPUState *cs)
{
    CPUS1C33State *env = cpu_env(cs);

    return cpu_test_interrupt(cs, S1C33_CPU_INTERRUPT_NMI) ||
           (cpu_test_interrupt(cs, CPU_INTERRUPT_HARD) &&
            s1c33_cpu_irq_ready(env));
}

static int s1c33_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return 0;
}

static void s1c33_cpu_reset_hold(Object *obj, ResetType type)
{
    CPUState *cs = CPU(obj);
    S1C33CPUClass *scc = S1C33_CPU_GET_CLASS(obj);
    CPUS1C33State *env = cpu_env(cs);

    if (scc->parent_phases.hold) {
        scc->parent_phases.hold(obj, type);
    }

    memset(env, 0, offsetof(CPUS1C33State, end_reset_fields));
}

static ObjectClass *s1c33_cpu_class_by_name(const char *cpu_model)
{
    ObjectClass *oc;
    char *typename;

    oc = object_class_by_name(cpu_model);
    if (oc != NULL && object_class_dynamic_cast(oc, TYPE_S1C33_CPU) != NULL) {
        return oc;
    }

    typename = g_strdup_printf(S1C33_CPU_TYPE_NAME("%s"), cpu_model);
    oc = object_class_by_name(typename);
    g_free(typename);

    return oc;
}

static const Property s1c33_cpu_properties[] = {
    DEFINE_PROP_BOOL("exit-on-halt", S1C33CPU, exit_on_halt, true),
    DEFINE_PROP_BOOL("trace-psr", S1C33CPU, trace_psr, false),
    DEFINE_PROP_BOOL("trace-calls", S1C33CPU, trace_calls, false),
    DEFINE_PROP_BOOL("trace-exec", S1C33CPU, trace_exec, false),
    DEFINE_PROP_BOOL("trace-mem", S1C33CPU, trace_mem, false),
    DEFINE_PROP_UINT32("trace-calls-start", S1C33CPU, trace_calls_start, 0),
    DEFINE_PROP_UINT32("trace-calls-end", S1C33CPU, trace_calls_end, 0),
    DEFINE_PROP_UINT32("trace-exec-start", S1C33CPU, trace_exec_start, 0),
    DEFINE_PROP_UINT32("trace-exec-end", S1C33CPU, trace_exec_end, 0),
    DEFINE_PROP_UINT32("trace-mem-start", S1C33CPU, trace_mem_start, 0),
    DEFINE_PROP_UINT32("trace-mem-end", S1C33CPU, trace_mem_end, 0),
    DEFINE_PROP_UINT32("trace-mem-pc-start", S1C33CPU, trace_mem_pc_start, 0),
    DEFINE_PROP_UINT32("trace-mem-pc-end", S1C33CPU, trace_mem_pc_end, 0),
};

static void s1c33_cpu_realize(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    S1C33CPUClass *scc = S1C33_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu_exec_realizefn(cs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }

    qemu_init_vcpu(cs);
    cpu_reset(cs);

    scc->parent_realize(dev, errp);
}

static void s1c33_cpu_disas_set_info(const CPUState *cpu,
                                     disassemble_info *info)
{
    info->endian = BFD_ENDIAN_LITTLE;
    info->print_insn = print_insn_s1c33;
}

static bool s1c33_cpu_tlb_fill(CPUState *cs, vaddr addr, int size,
                               MMUAccessType access_type, int mmu_idx,
                               bool probe, uintptr_t retaddr)
{
    hwaddr page = addr & TARGET_PAGE_MASK;

    tlb_set_page(cs, page, page, PAGE_READ | PAGE_WRITE | PAGE_EXEC,
                 mmu_idx, TARGET_PAGE_SIZE);
    return true;
}

static bool s1c33_cpu_load_u32(CPUState *cs, uint32_t addr, uint32_t *value)
{
    uint8_t buf[4];

    if (cpu_memory_rw_debug(cs, addr, buf, sizeof(buf), false) < 0) {
        return false;
    }

    *value = ldl_le_p(buf);
    return true;
}

static bool s1c33_cpu_store_u32(CPUState *cs, uint32_t addr, uint32_t value)
{
    uint8_t buf[4];

    stl_le_p(buf, value);
    return cpu_memory_rw_debug(cs, addr, buf, sizeof(buf), true) == 0;
}

static void s1c33_cpu_enter_trap(CPUState *cs, const char *name,
                                 uint8_t vector, uint8_t level,
                                 bool maskable)
{
    CPUS1C33State *env = cpu_env(cs);
    uint32_t old_pc = env->pc;
    uint32_t old_psr = env->psr;
    uint32_t vector_addr;
    uint32_t handler;

    vector_addr = (env->ttbr & S1C33_TTBR_MASK) + vector * 4;
    if (!s1c33_cpu_load_u32(cs, vector_addr, &handler)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s1c33: %s vector read failed ttbr=0x%08x "
                      "vector=%u addr=0x%08x\n",
                      name, env->ttbr, vector, vector_addr);
        return;
    }

    env->sp -= 4;
    if (!s1c33_cpu_store_u32(cs, env->sp, old_pc)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s1c33: %s PC push failed sp=0x%08x pc=0x%08x\n",
                      name, env->sp, old_pc);
        return;
    }

    env->sp -= 4;
    if (!s1c33_cpu_store_u32(cs, env->sp, old_psr)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s1c33: %s PSR push failed sp=0x%08x psr=0x%08x\n",
                      name, env->sp, old_psr);
        return;
    }

    env->psr = old_psr & ~S1C33_PSR_IE;
    if (maskable) {
        env->psr = (env->psr & ~S1C33_PSR_IL_MASK) |
                   ((uint32_t)(level & 0xf) << S1C33_PSR_IL_SHIFT);
    }
    env->pc = handler & ~1u;
    env->in_sleep = false;
    cs->halted = 0;

    qemu_log_mask(CPU_LOG_INT,
                  "s1c33: %s vector=%u level=%u table=0x%08x "
                  "handler=0x%08x old_pc=0x%08x old_psr=0x%08x sp=0x%08x\n",
                  name, vector, level, env->ttbr & S1C33_TTBR_MASK, env->pc,
                  old_pc, old_psr, env->sp);
}

void s1c33_cpu_do_interrupt(CPUState *cs)
{
    CPUS1C33State *env = cpu_env(cs);

    if (!s1c33_cpu_irq_ready(env)) {
        return;
    }

    s1c33_cpu_enter_trap(cs, "IRQ", env->irq_vector, env->irq_level, true);
}

bool s1c33_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    if ((interrupt_request & S1C33_CPU_INTERRUPT_NMI) != 0) {
        cpu_reset_interrupt(cs, S1C33_CPU_INTERRUPT_NMI);
        s1c33_cpu_enter_trap(cs, "NMI", S1C33_TRAP_NMI_VECTOR, 0, false);
        return true;
    }

    if ((interrupt_request & CPU_INTERRUPT_HARD) != 0 &&
        s1c33_cpu_irq_ready(cpu_env(cs))) {
        s1c33_cpu_do_interrupt(cs);
        return true;
    }

    return false;
}

void s1c33_cpu_set_irq(CPUState *cs, bool pending, uint8_t vector,
                       uint8_t level)
{
    CPUS1C33State *env = cpu_env(cs);

    env->irq_pending = pending;
    if (pending) {
        env->irq_vector = vector;
        env->irq_level = level & 0xf;
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        env->irq_vector = 0;
        env->irq_level = 0;
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

void s1c33_cpu_raise_nmi(CPUState *cs)
{
    cpu_interrupt(cs, S1C33_CPU_INTERRUPT_NMI);
}

void s1c33_cpu_resume_from_sleep(CPUState *cs, const char *reason)
{
    CPUS1C33State *env = cpu_env(cs);

    if (!env->in_sleep) {
        return;
    }

    env->in_sleep = false;
    cs->halted = 0;
    qemu_log_mask(CPU_LOG_INT,
                  "s1c33: resume from sleep reason=%s pc=0x%08x "
                  "psr=0x%08x\n",
                  reason != NULL ? reason : "external-clock",
                  env->pc, env->psr);
    cpu_resume(cs);
}

hwaddr s1c33_cpu_get_phys_page_debug(CPUState *cs, vaddr addr)
{
    return addr;
}

void s1c33_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPUS1C33State *env = cpu_env(cs);
    int i;

    qemu_fprintf(f,
                 "pc=0x%08x psr=0x%08x sp=0x%08x ttbr=0x%08x "
                 "irq=%u vector=%u level=%u\n",
                 env->pc, env->psr, env->sp, env->ttbr,
                 env->irq_pending, env->irq_vector, env->irq_level);
    for (i = 0; i < S1C33_NUM_REGS; i += 4) {
        qemu_fprintf(f,
                     "r%-2d=0x%08x r%-2d=0x%08x r%-2d=0x%08x r%-2d=0x%08x\n",
                     i, env->regs[i], i + 1, env->regs[i + 1],
                     i + 2, env->regs[i + 2], i + 3, env->regs[i + 3]);
    }
}

#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps s1c33_sysemu_ops = {
    .has_work = s1c33_cpu_has_work,
    .get_phys_page_debug = s1c33_cpu_get_phys_page_debug,
};

static const TCGCPUOps s1c33_tcg_ops = {
    .guest_default_memory_order = TCG_MO_ALL,
    .mttcg_supported = false,
    .initialize = s1c33_translate_init,
    .translate_code = s1c33_translate_code,
    .get_tb_cpu_state = s1c33_get_tb_cpu_state,
    .synchronize_from_tb = s1c33_cpu_synchronize_from_tb,
    .restore_state_to_opc = s1c33_restore_state_to_opc,
    .mmu_index = s1c33_cpu_mmu_index,
    .tlb_fill = s1c33_cpu_tlb_fill,
    .pointer_wrap = cpu_pointer_wrap_uint32,
    .cpu_exec_interrupt = s1c33_cpu_exec_interrupt,
    .cpu_exec_halt = s1c33_cpu_has_work,
    .cpu_exec_reset = cpu_reset,
    .do_interrupt = s1c33_cpu_do_interrupt,
};

static void s1c33_cpu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    CPUClass *cc = CPU_CLASS(oc);
    S1C33CPUClass *scc = S1C33_CPU_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_parent_realize(dc, s1c33_cpu_realize,
                                    &scc->parent_realize);
    device_class_set_props(dc, s1c33_cpu_properties);
    resettable_class_set_parent_phases(rc, NULL, s1c33_cpu_reset_hold, NULL,
                                       &scc->parent_phases);

    cc->class_by_name = s1c33_cpu_class_by_name;
    cc->dump_state = s1c33_cpu_dump_state;
    cc->set_pc = s1c33_cpu_set_pc;
    cc->get_pc = s1c33_cpu_get_pc;
    cc->sysemu_ops = &s1c33_sysemu_ops;
    cc->gdb_read_register = s1c33_cpu_gdb_read_register;
    cc->gdb_write_register = s1c33_cpu_gdb_write_register;
    cc->disas_set_info = s1c33_cpu_disas_set_info;
    cc->gdb_core_xml_file = "s1c33-core.xml";
    cc->tcg_ops = &s1c33_tcg_ops;
}

static const TypeInfo s1c33_cpu_type_info[] = {
    {
        .name = TYPE_S1C33_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(S1C33CPU),
        .instance_align = __alignof(S1C33CPU),
        .abstract = true,
        .class_size = sizeof(S1C33CPUClass),
        .class_init = s1c33_cpu_class_init,
    },
    {
        .name = TYPE_C33L05_CPU,
        .parent = TYPE_S1C33_CPU,
    },
};

DEFINE_TYPES(s1c33_cpu_type_info)
