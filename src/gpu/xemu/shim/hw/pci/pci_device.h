/* Stand-in for QEMU's hw/pci/pci_device.h: a complete PCIDevice, contents unused.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "hw/hw.h"

struct PCIDevice { uint8_t config[256]; };
