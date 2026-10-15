/*
 * Link check for mksm_gpu (plan 2.H.3a, CTest gpu_link). Built with MSVC cl and linked with
 * link.exe against the clang-cl-built library, the same way the game exe will be: linking at
 * all proves there are no unresolved symbols, running proves the null renderer registered.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "mksm_gpu.h"

int main(void)
{
    return mksm_gpu_selftest();
}
