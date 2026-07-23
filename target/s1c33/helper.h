/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

DEF_HELPER_2(s1c33_record_tb, void, env, i32)
DEF_HELPER_5(s1c33_trace_branch, void, env, i32, i32, i32, i32)
DEF_HELPER_6(s1c33_trace_mem, void, env, i32, i32, i32, i32, i32)
DEF_HELPER_4(s1c33_write_psr, void, env, i32, i32, i32)
DEF_HELPER_3(s1c33_div, void, env, i32, i32)
DEF_HELPER_4(s1c33_mul, void, env, i32, i32, i32)
DEF_HELPER_2(s1c33_brk, noreturn, env, i32)
DEF_HELPER_2(s1c33_unimplemented, noreturn, env, i32)
DEF_HELPER_2(s1c33_sleep, noreturn, env, i32)
DEF_HELPER_2(s1c33_halt, noreturn, env, i32)
