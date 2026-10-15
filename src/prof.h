/* RECOMP_PROF sampling profiler (src/prof.c). */
#pragma once

/* Starts the sampler thread if RECOMP_PROF=<start_ms>,<dur_ms>[,<period_us>] is set. Needs
 * dbghelp's SymInitialize to have run (symbols are resolved when the window ends). */
void prof_start_from_env(void);
