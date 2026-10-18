/* Stand-in for QEMU's qemu/thread.h on Win32 primitives (on POSIX: the toolkit's
 * win32_compat, see host_win32.h).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "qemu/osdep.h"
#include "host_win32.h"

typedef struct QemuMutex { CRITICAL_SECTION cs; } QemuMutex;
typedef struct QemuCond { CONDITION_VARIABLE cv; } QemuCond;
/* Manual-reset event, like QEMU's QemuEvent. */
typedef struct QemuEvent { HANDLE h; } QemuEvent;
typedef struct QemuThread { HANDLE h; } QemuThread;

static inline void qemu_mutex_init(QemuMutex *m) { InitializeCriticalSection(&m->cs); }
static inline void qemu_mutex_destroy(QemuMutex *m) { DeleteCriticalSection(&m->cs); }
static inline void qemu_mutex_lock(QemuMutex *m) { EnterCriticalSection(&m->cs); }
static inline void qemu_mutex_unlock(QemuMutex *m) { LeaveCriticalSection(&m->cs); }

static inline void qemu_cond_init(QemuCond *c) { InitializeConditionVariable(&c->cv); }
static inline void qemu_cond_destroy(QemuCond *c) { (void)c; }
static inline void qemu_cond_signal(QemuCond *c) { WakeConditionVariable(&c->cv); }
static inline void qemu_cond_broadcast(QemuCond *c) { WakeAllConditionVariable(&c->cv); }
static inline void qemu_cond_wait(QemuCond *c, QemuMutex *m)
{
    SleepConditionVariableCS(&c->cv, &m->cs, INFINITE);
}

static inline void qemu_event_init(QemuEvent *e, bool init)
{
    e->h = CreateEventA(NULL, TRUE, init, NULL);
}
static inline void qemu_event_destroy(QemuEvent *e) { CloseHandle(e->h); }
static inline void qemu_event_set(QemuEvent *e) { SetEvent(e->h); }
static inline void qemu_event_reset(QemuEvent *e) { ResetEvent(e->h); }
static inline void qemu_event_wait(QemuEvent *e) { WaitForSingleObject(e->h, INFINITE); }

/* QEMU threads: only joinable ones are created (the GL shader cache's disk threads). */
#define QEMU_THREAD_JOINABLE 0
#define QEMU_THREAD_DETACHED 1
typedef struct QemuThreadStart {
    void *(*fn)(void *);
    void *arg;
    const char *name;   /* POSIX: the thread names itself */
} QemuThreadStart;
static DWORD WINAPI qemu_thread_trampoline(LPVOID p)
{
    QemuThreadStart start = *(QemuThreadStart *)p;
    free(p);
#ifndef _WIN32
    if (start.name) {
        mksm_name_this_thread(start.name);
    }
#endif
    start.fn(start.arg);
    return 0;
}
static inline void qemu_thread_create(QemuThread *t, const char *name, void *(*fn)(void *),
                                      void *arg, int mode)
{
    QemuThreadStart *start = (QemuThreadStart *)malloc(sizeof(*start));
    start->fn = fn;
    start->arg = arg;
    start->name = name;
    t->h = CreateThread(NULL, 0, qemu_thread_trampoline, start, 0, NULL);
    assert(t->h);
#ifdef _WIN32
    wchar_t wname[64];
    if (name && MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64)) {
        SetThreadDescription(t->h, wname);   /* for profilers and debuggers */
    }
#endif
    if (mode == QEMU_THREAD_DETACHED) {
        CloseHandle(t->h);
        t->h = NULL;
    }
}
static inline void *qemu_thread_join(QemuThread *t)
{
    if (t->h) {
        WaitForSingleObject(t->h, INFINITE);
        CloseHandle(t->h);
        t->h = NULL;
    }
    return NULL;
}
