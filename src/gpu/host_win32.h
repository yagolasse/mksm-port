/*
 * The Win32 host primitives the GPU glue uses (events, threads, critical sections, interlocked
 * ops, waitable timers): <windows.h> on Windows; on POSIX (plan H.5c2c) the toolkit's
 * win32_compat layer, which implements the same subset on pthreads and is linked into the game
 * exe already. The few calls it lacks are filled in here.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdio.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#define mksm_mkdir(path) _mkdir(path)
#else
#include "win32_compat.h"
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#define _stricmp strcasecmp
#define _unlink unlink
#define mksm_mkdir(path) mkdir((path), 0755)

/* Thread names are for profilers and debuggers only; Linux has no handle -> pthread_t lookup
 * here, so a name is set by the thread itself (mksm_name_this_thread). */
static inline long SetThreadDescription(HANDLE h, const wchar_t *name)
{
    (void)h;
    (void)name;
    return 0;
}
#endif

/* Names the calling thread (shown by top -H, gdb, perf). */
static inline void mksm_name_this_thread(const char *name)
{
#if defined(__linux__)
    char buf[16];               /* the kernel keeps 15 characters */
    snprintf(buf, sizeof(buf), "%s", name);
    pthread_setname_np(pthread_self(), buf);
#else
    (void)name;
#endif
}
