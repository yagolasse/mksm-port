/* Stand-in for QEMU's cpu.h (the run-on-cpu bits PGRAPH's renderer switch uses).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "qemu/osdep.h"

struct CPUState { int cpu_index; };

typedef union { void *host_ptr; int host_int; } run_on_cpu_data;
#define RUN_ON_CPU_HOST_PTR(p) ((run_on_cpu_data){ .host_ptr = (p) })
typedef void (*run_on_cpu_func)(CPUState *cpu, run_on_cpu_data data);

CPUState *qemu_get_cpu(int index);
void async_safe_run_on_cpu(CPUState *cpu, run_on_cpu_func func, run_on_cpu_data data);
