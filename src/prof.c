/*
 * RECOMP_PROF: a sampling profiler over every thread of the process (plan 2.H.8).
 *
 *   RECOMP_PROF=<start_ms>,<dur_ms>[,<period_us>]
 *
 * Waits start_ms after prof_start_from_env(), then every period suspends each thread,
 * unwinds its stack (x64 unwind data, RtlVirtualUnwind) and resumes it. (Skipping threads
 * whose cycle count didn't move doesn't work: the suspend itself runs on the target and
 * charges it cycles.) At the end it prints, per thread with >= 2% CPU in the window: the CPU
 * time, its start routine, and the functions with the most exclusive (innermost frame) and inclusive
 * (anywhere on the stack) samples, as module!symbol from the PDBs dbghelp can find.
 *
 * Nothing is allocated or locked while a thread is suspended (the frames go into a buffer
 * set up front), so a thread holding the heap lock can't deadlock the sampler.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prof.h"

#define PROF_MAX_THREADS 128
#define PROF_MAX_FRAMES 24
#define PROF_TOP 25

typedef struct {
    DWORD tid;
    HANDLE h;
    ULONG64 cpu_start;      /* kernel + user, 100 ns */
    ULONG64 start_routine;
    const NT_TIB *tib;      /* for the stack bounds */
    unsigned long samples;
} ProfThread;

typedef struct {
    unsigned char thread;
    unsigned char depth;
    DWORD64 pc[PROF_MAX_FRAMES];
} ProfSample;

static ProfThread s_threads[PROF_MAX_THREADS];
static unsigned s_nthreads;
static ProfSample *s_samples;
static size_t s_nsamples, s_cap;
static DWORD s_start_ms, s_dur_ms, s_period_us;

static ULONG64 thread_cpu(HANDLE h)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) {
        return 0;
    }
    return ((ULONG64)k.dwHighDateTime << 32 | k.dwLowDateTime) +
           ((ULONG64)u.dwHighDateTime << 32 | u.dwLowDateTime);
}

typedef LONG(NTAPI *QueryThreadFn)(HANDLE, int, PVOID, ULONG, PULONG);

static QueryThreadFn query_thread(void)
{
    static QueryThreadFn query;
    if (!query) {
        query = (QueryThreadFn)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                              "NtQueryInformationThread");
    }
    return query;
}

static ULONG64 thread_start_routine(HANDLE h)
{
    ULONG64 start = 0;
    if (query_thread()) {
        query_thread()(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), NULL);
    }
    return start;
}

/* The thread's TEB starts with its NT_TIB (stack base and limit). */
static const NT_TIB *thread_tib(HANDLE h)
{
    struct {
        LONG exit_status;
        PVOID teb;
        ULONG_PTR rest[4];
    } basic = { 0 };
    if (!query_thread() ||
        query_thread()(h, 0 /* ThreadBasicInformation */, &basic, sizeof(basic), NULL) < 0) {
        return NULL;
    }
    return (const NT_TIB *)basic.teb;
}

/* Adds threads that appeared since the last refresh. */
static void refresh_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    THREADENTRY32 te = { .dwSize = sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
            continue;
        }
        unsigned i;
        for (i = 0; i < s_nthreads && s_threads[i].tid != te.th32ThreadID; i++) {
        }
        if (i < s_nthreads || s_nthreads == PROF_MAX_THREADS) {
            continue;
        }
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                              THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) {
            continue;
        }
        ProfThread *t = &s_threads[s_nthreads++];
        t->tid = te.th32ThreadID;
        t->h = h;
        t->cpu_start = thread_cpu(h);
        t->start_routine = thread_start_routine(h);
        t->tib = thread_tib(h);
    }
    CloseHandle(snap);
}

static void sample_thread(unsigned idx)
{
    ProfThread *t = &s_threads[idx];
    if (s_nsamples == s_cap || !t->tib) {
        return;
    }
    if (SuspendThread(t->h) == (DWORD)-1) {
        return;
    }
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(t->h, &ctx)) {
        ProfSample *s = &s_samples[s_nsamples];
        s->thread = (unsigned char)idx;
        s->depth = 0;
        /* Every read the unwind makes is at rsp or above, so it stays safe while rsp is
         * inside the thread's stack; a frame without unwind data ends the walk unless it is
         * the innermost (a leaf, whose return address is at [rsp]). */
        DWORD64 lo = (DWORD64)t->tib->StackLimit, hi = (DWORD64)t->tib->StackBase;
        for (int d = 0; d < PROF_MAX_FRAMES && ctx.Rip; d++) {
            s->pc[s->depth++] = ctx.Rip;
            if (ctx.Rsp < lo || ctx.Rsp + 512 > hi) {
                break;
            }
            DWORD64 base;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
            if (!fn) {
                if (d) {
                    break;
                }
                ctx.Rip = *(DWORD64 *)ctx.Rsp;
                ctx.Rsp += 8;
            } else {
                PVOID handler_data;
                DWORD64 frame;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handler_data,
                                 &frame, NULL);
            }
        }
        s_nsamples++;
        t->samples++;
    }
    ResumeThread(t->h);
}

/* --- report ---------------------------------------------------------------------------- */

typedef struct {
    DWORD64 key;            /* symbol start (or the pc itself when unknown) */
    char name[96];
} SymEntry;

typedef struct {
    DWORD64 pc;
    unsigned sym;
} PcEntry;

static SymEntry *s_syms;
static unsigned s_nsyms, s_syms_cap;
static PcEntry *s_pcs;
static size_t s_pcs_cap;    /* power of two, open addressing */

static unsigned sym_for_key(DWORD64 key, const char *name)
{
    for (unsigned i = 0; i < s_nsyms; i++) {
        if (s_syms[i].key == key) {
            return i;
        }
    }
    if (s_nsyms == s_syms_cap) {
        s_syms_cap = s_syms_cap ? s_syms_cap * 2 : 1024;
        s_syms = (SymEntry *)realloc(s_syms, s_syms_cap * sizeof(*s_syms));
    }
    SymEntry *e = &s_syms[s_nsyms];
    e->key = key;
    snprintf(e->name, sizeof(e->name), "%s", name);
    return s_nsyms++;
}

static unsigned resolve(DWORD64 pc)
{
    size_t mask = s_pcs_cap - 1, i = (size_t)(pc * 0x9E3779B97F4A7C15ull >> 20) & mask;
    for (; s_pcs[i].pc; i = (i + 1) & mask) {
        if (s_pcs[i].pc == pc) {
            return s_pcs[i].sym;
        }
    }
    HANDLE proc = GetCurrentProcess();
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    memset(buf, 0, sizeof(SYMBOL_INFO));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    IMAGEHLP_MODULE64 mod = { .SizeOfStruct = sizeof(mod) };
    const char *mname = SymGetModuleInfo64(proc, pc, &mod) ? mod.ModuleName : "?";
    char name[96];
    DWORD64 disp, key;
    if (SymFromAddr(proc, pc, &disp, sym)) {
        snprintf(name, sizeof(name), "%s!%s", mname, sym->Name);
        key = sym->Address;
    } else {
        snprintf(name, sizeof(name), "%s!0x%llX", mname, (unsigned long long)pc);
        key = pc;
    }
    unsigned id = sym_for_key(key, name);
    s_pcs[i].pc = pc;
    s_pcs[i].sym = id;
    return id;
}

static unsigned long *s_sort_counts;

static int by_count_desc(const void *a, const void *b)
{
    unsigned long ca = s_sort_counts[*(const unsigned *)a];
    unsigned long cb = s_sort_counts[*(const unsigned *)b];
    return ca < cb ? 1 : ca > cb ? -1 : 0;
}

static void print_top(const char *title, unsigned long *counts, unsigned long total)
{
    unsigned *order = (unsigned *)malloc(s_nsyms * sizeof(unsigned));
    for (unsigned i = 0; i < s_nsyms; i++) {
        order[i] = i;
    }
    s_sort_counts = counts;
    qsort(order, s_nsyms, sizeof(unsigned), by_count_desc);
    fprintf(stderr, "    %s:\n", title);
    for (unsigned i = 0; i < s_nsyms && i < PROF_TOP && counts[order[i]]; i++) {
        fprintf(stderr, "      %5.1f%%  %6lu  %s\n", 100.0 * counts[order[i]] / total,
                counts[order[i]], s_syms[order[i]].name);
    }
    free(order);
}

static void report(DWORD wall_ms)
{
    SymRefreshModuleList(GetCurrentProcess());   /* DLLs loaded after SymInitialize */
    s_pcs_cap = 1u << 18;
    s_pcs = (PcEntry *)calloc(s_pcs_cap, sizeof(*s_pcs));
    unsigned *ids = (unsigned *)malloc(s_nsamples * PROF_MAX_FRAMES * sizeof(unsigned));
    for (size_t i = 0; i < s_nsamples; i++) {
        for (unsigned d = 0; d < s_samples[i].depth; d++) {
            ids[i * PROF_MAX_FRAMES + d] = resolve(s_samples[i].pc[d]);
        }
    }
    fprintf(stderr, "[PROF] %zu samples over %lu ms (period %lu us, %u threads seen)%s\n",
            s_nsamples, wall_ms, s_period_us, s_nthreads,
            s_nsamples == s_cap ? ", buffer full" : "");
    unsigned long *excl = (unsigned long *)calloc(s_nsyms, sizeof(unsigned long));
    unsigned long *incl = (unsigned long *)calloc(s_nsyms, sizeof(unsigned long));
    unsigned *seen = (unsigned *)calloc(s_nsyms, sizeof(unsigned));
    for (unsigned t = 0; t < s_nthreads; t++) {
        ProfThread *th = &s_threads[t];
        ULONG64 cpu = thread_cpu(th->h) - th->cpu_start;
        if (!th->samples || cpu / 1e4 < wall_ms * 0.02) {
            continue;   /* idle threads: their samples are all in a wait */
        }
        memset(excl, 0, s_nsyms * sizeof(unsigned long));
        memset(incl, 0, s_nsyms * sizeof(unsigned long));
        unsigned stamp = 0;
        memset(seen, 0, s_nsyms * sizeof(unsigned));
        for (size_t i = 0; i < s_nsamples; i++) {
            if (s_samples[i].thread != t || !s_samples[i].depth) {
                continue;
            }
            const unsigned *f = &ids[i * PROF_MAX_FRAMES];
            excl[f[0]]++;
            stamp++;
            for (unsigned d = 0; d < s_samples[i].depth; d++) {
                if (seen[f[d]] != stamp) {
                    seen[f[d]] = stamp;
                    incl[f[d]]++;
                }
            }
        }
        unsigned start = th->start_routine ? resolve(th->start_routine) : 0;
        fprintf(stderr, "[PROF] thread %lu (%s): %lu samples, CPU %.0f ms = %.0f%% of the window\n",
                th->tid, th->start_routine ? s_syms[start].name : "?", th->samples,
                cpu / 1e4, wall_ms ? cpu / 1e4 * 100.0 / wall_ms : 0.0);
        print_top("exclusive", excl, th->samples);
        print_top("inclusive", incl, th->samples);
    }
    free(seen);
    free(incl);
    free(excl);
    free(ids);
}

static DWORD WINAPI prof_thread(LPVOID arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    Sleep(s_start_ms);
    s_nthreads = 0;
    refresh_threads();
    for (unsigned i = 0; i < s_nthreads; i++) {
        s_threads[i].cpu_start = thread_cpu(s_threads[i].h);
    }
    fprintf(stderr, "[PROF] sampling %u threads for %lu ms\n", s_nthreads, s_dur_ms);

    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    DWORD t0 = GetTickCount(), last_refresh = t0;
    while (GetTickCount() - t0 < s_dur_ms) {
        for (unsigned i = 0; i < s_nthreads; i++) {
            sample_thread(i);
        }
        if (GetTickCount() - last_refresh > 500) {
            last_refresh = GetTickCount();
            refresh_threads();
        }
        LARGE_INTEGER due = { .QuadPart = -(LONGLONG)s_period_us * 10 };
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
    }
    report(GetTickCount() - t0);
    return 0;
}

void prof_start_from_env(void)
{
    const char *e = getenv("RECOMP_PROF");
    if (!e || !e[0]) {
        return;
    }
    unsigned long start = 0, dur = 10000, period = 1000;
    if (sscanf(e, "%lu,%lu,%lu", &start, &dur, &period) < 1) {
        fprintf(stderr, "[PROF] RECOMP_PROF=%s: expected <start_ms>,<dur_ms>[,<period_us>]\n", e);
        return;
    }
    s_start_ms = start;
    s_dur_ms = dur;
    s_period_us = period ? period : 1000;
    /* One sample per thread per pass; with ~25 threads a pass takes ~2.5 ms (more than the
     * 1 ms default period), so 16 per period is plenty. */
    s_cap = (size_t)dur * 1000 / s_period_us * 16;
    s_samples = (ProfSample *)malloc(s_cap * sizeof(ProfSample));
    if (!s_samples) {
        fprintf(stderr, "[PROF] can't allocate %zu samples\n", s_cap);
        return;
    }
    CloseHandle(CreateThread(NULL, 0, prof_thread, NULL, 0, NULL));
    fprintf(stderr, "[PROF] armed: start %lu ms, %lu ms, period %lu us\n", start, dur,
            s_period_us);
}
