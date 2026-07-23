/*
 * Epson S1C33 CPU QOM definitions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_S1C33_CPU_QOM_H
#define TARGET_S1C33_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_S1C33_CPU "s1c33-cpu"
#define TYPE_C33L05_CPU S1C33_CPU_TYPE_NAME("c33l05")

OBJECT_DECLARE_CPU_TYPE(S1C33CPU, S1C33CPUClass, S1C33_CPU)

#define S1C33_CPU_TYPE_SUFFIX "-" TYPE_S1C33_CPU
#define S1C33_CPU_TYPE_NAME(model) model S1C33_CPU_TYPE_SUFFIX

#endif
