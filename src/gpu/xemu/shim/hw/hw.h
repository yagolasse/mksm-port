/* Stand-in for QEMU's hw/hw.h (+ hw/irq.h). SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "qemu/osdep.h"
#include "exec/memory.h"

typedef struct IRQState *qemu_irq;
