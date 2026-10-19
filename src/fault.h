/**
 * Crash diagnostics (plan B14, split out of main.c): the Win32 vectored exception handler and
 * the POSIX SIGSEGV/SIGBUS handler. Both hand trapped hardware registers (APU, AC'97, OHCI) to
 * the toolkit first and print the guest context of any other fault.
 */
#ifndef MKSM_FAULT_H
#define MKSM_FAULT_H

#include "paths.h"      /* the platform headers */

#ifdef _WIN32
LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep);
#else
void posix_fault_install(void);
#endif

#endif
