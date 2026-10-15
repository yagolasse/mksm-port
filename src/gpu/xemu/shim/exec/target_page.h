/* Stand-in for QEMU's exec/target_page.h. SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE (1 << TARGET_PAGE_BITS)
#define TARGET_PAGE_MASK (~(TARGET_PAGE_SIZE - 1))
