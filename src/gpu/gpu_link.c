/*
 * Link check for mksm_gpu (plan 2.H.3a/2.H.4a, CTests gpu_link and gpu_link_gl). Built with
 * MSVC cl and linked with link.exe against the clang-cl-built library, the same way the game
 * exe is: linking at all proves there are no unresolved symbols, running proves the renderer
 * registered and comes up. `gpu_link` tests the null renderer, `gpu_link gl` OpenGL.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <string.h>

#include "mksm_gpu.h"

int main(int argc, char **argv)
{
    return mksm_gpu_selftest(argc > 1 && strcmp(argv[1], "gl") == 0);
}
