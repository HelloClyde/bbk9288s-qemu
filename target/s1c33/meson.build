# SPDX-License-Identifier: GPL-2.0-or-later

s1c33_ss = ss.source_set()
s1c33_ss.add(files(
  'translate.c',
  'helper.c',
  'cpu.c',
  'disas.c',
))

s1c33_common_system_ss = ss.source_set()
s1c33_common_system_ss.add(files('gdbstub.c'))

target_arch += {'s1c33': s1c33_ss}
target_common_system_arch += {'s1c33': s1c33_common_system_ss}
