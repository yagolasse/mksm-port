/*
 * Interface of mksm_gpu (xemu's PGRAPH, built with clang-cl) for code built with MSVC cl.
 * Plain C only: the xemu headers need GNU C, so cl-built code includes this, not them.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up xemu's PGRAPH with the null renderer on a scratch NV2AState, prints one line to
 * stdout, tears it down. Returns 0 on success. Used by the gpu_link check (plan 2.H.3a). */
int mksm_gpu_selftest(void);

#ifdef __cplusplus
}
#endif
