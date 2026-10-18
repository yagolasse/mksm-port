/**
 * MK Shaolin Monks - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - The game dir comes from argv[1], MKSM_GAME_DIR (or settings.ini's game_dir=), or
 *     "game"; else the first-launch setup asks for it (see resolve_game_dir)
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       MK Shaolin Monks
 *   Title ID:    0x4D570029 (built 2005-08-23, XDK 5849)
 *   Base addr:   0x00010000
 *   Entry point: 0x000F40CA
 *   Code size:   ~3797 KB (.text)
 *   Sections:    16 (.text D3D XGRPH DSOUND D3DX PSFD00 WMADEC XPP .rdata
 *                .data .idata DOLBY $$XTINFO $$XTIMAGE $$XSIMAGE .XTLID)
 *   Kernel imports: 152
 */

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <dbghelp.h>
#include <io.h>
#include <fcntl.h>
#else
/* POSIX (plan H.5c2a): the toolkit's Win32 vocabulary (threads, waitable timers, QPC) and
 * the libc calls that stand in for the rest. */
#define _GNU_SOURCE             /* REG_RIP, dladdr */
#include "win32_compat.h"
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>
#include <xbox/thread_name.h>

#include "prof.h"
#include "settings.h"
#include "setup.h"

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

#define YOUR_GAME_ENTRY_POINT   0x000F40CA  /* XBE entry point VA */

/* Game directory: the extracted disc root, holding default.xbe and the
 * data files. Taken from the first command-line argument, else the
 * MKSM_GAME_DIR environment variable (or the settings file's game_dir=),
 * else "game" in the working directory. Made absolute by resolve_game_dir().
 * Without a default.xbe there (and no argument / variable of the user's),
 * the first-launch setup asks for the disc (src/setup.c, plan B10b2). */
#define DEFAULT_GAME_DIR        "game"

#ifdef _WIN32
#define DIRSEP          "\\"
#define mksm_argc       __argc
#define mksm_argv       __argv
#else
#undef MAX_PATH
#define MAX_PATH        PATH_MAX
#define DIRSEP          "/"
static int mksm_argc;
static char **mksm_argv;
#endif

/* ── Host file system (Win32 calls, POSIX equivalents) ──────── */

#ifdef _WIN32
static void full_path(const char *in, char *out)
{
    if (!GetFullPathNameA(in, MAX_PATH, out, NULL))
        snprintf(out, MAX_PATH, "%s", in);
}
static bool path_exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static bool path_is_dir(const char *p)
{
    DWORD fa = GetFileAttributesA(p);
    return fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_DIRECTORY);
}
/* The exe's folder; false if unknown. */
static bool exe_dir(char *out)
{
    DWORD n = GetModuleFileNameA(NULL, out, MAX_PATH);
    char *slash = n && n < MAX_PATH ? strrchr(out, '\\') : NULL;
    if (!slash)
        return false;
    *slash = '\0';
    return true;
}
static bool move_path(const char *from, const char *to) { return MoveFileExA(from, to, 0) != 0; }
static void make_dirs(const char *dir) { SHCreateDirectoryExA(NULL, dir, NULL); }
static int env_put(const char *name, const char *value) { return _putenv_s(name, value); }
#else
static void full_path(const char *in, char *out)
{
    char cwd[MAX_PATH];
    if (in[0] != '/' && getcwd(cwd, sizeof(cwd)))
        snprintf(out, MAX_PATH, "%s/%s", cwd, in);
    else
        snprintf(out, MAX_PATH, "%s", in);
}
static bool path_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static bool path_is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static bool exe_dir(char *out)
{
    ssize_t n = readlink("/proc/self/exe", out, MAX_PATH - 1);
    char *slash;
    if (n <= 0)
        return false;
    out[n] = '\0';
    slash = strrchr(out, '/');
    if (!slash)
        return false;
    *slash = '\0';
    return true;
}
static bool move_path(const char *from, const char *to) { return rename(from, to) == 0; }
static void make_dirs(const char *dir)
{
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}
static int env_put(const char *name, const char *value) { return setenv(name, value, 1); }
#define InterlockedAnd(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
#endif

static char g_game_dir[MAX_PATH];
static char g_xbe_path[MAX_PATH];

/* Base directory: the emulated hard disk (partition images, T:/Z: caches) and
 * settings.ini (plan B7b1). %LOCALAPPDATA%\mksm_recomp: not the toolkit default
 * (%LOCALAPPDATA%\xboxrecomp), which every xboxrecomp title shares, and not the
 * game dir, which may be read-only (a Steam install).
 * Save directory: the title's UDATA/TDATA (the saves) alone, <base>\saves, so a
 * player can sync the saves without the settings. MKSM_SAVE_DIR (or the
 * settings file's save_dir=) moves it. It goes to the toolkit as
 * RECOMP_SAVE_DIR, which moves just those two; one already set wins. Both made
 * absolute by resolve_base_dir() / resolve_save_dir(). */
#define SAVE_DIR_NAME           "mksm_recomp"

static char g_base_dir[MAX_PATH];
static char g_save_dir[MAX_PATH];
static bool g_save_dir_toolkit = true;  /* RECOMP_SAVE_DIR = g_save_dir (we set it) */

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);
/* Flat dispatch table (generated recomp_dispatch.c; declared in recomp_types.h, which main.c
 * does not include). */
extern int recomp_dispatch_init(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
#ifdef _WIN32
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);
#else
    /* POSIX: the exe exports its symbols (ENABLE_EXPORTS: -rdynamic), so dladdr finds the
     * generated sub_XXXXXXXX the address is in. */
    Dl_info info;
    if (dladdr(rip, &info) && info.dli_sname)
        fprintf(stderr, "  in %s+0x%llX\n", info.dli_sname,
                (unsigned long long)((uintptr_t)rip - (uintptr_t)info.dli_saddr));
#endif

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
/* GPU renderer (plan 2.H): RECOMP_PB_GPU=1 hands every decoded pushbuffer method to xemu's
 * PGRAPH (src/gpu) instead of the toolkit's software executor. RECOMP_PB_EXEC=1 is still
 * needed: it is what makes the ack thread decode the pushbuffer at all.
 * RECOMP_PB_GPU_RENDERER=null picks xemu's null renderer instead of OpenGL;
 * RECOMP_PB_GPU_VBLANK=<Hz> sets the flip pacing (default 59.94, 0 = uncapped). */
#ifdef MKSM_HAVE_GPU
#include "gpu/mksm_gpu.h"
#ifdef MKSM_HAVE_MENU
#include "ui/menu.h"
#endif

typedef void (*nv2a_pb_method_hook_fn)(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_set_method_hook(nv2a_pb_method_hook_fn fn);
extern int xbox_Nv2aSoftwareMethod(uint32_t param);

/* Software notifies go to the toolkit, which does what D3D's interrupt handler would
 * (method 5 sets the event D3D's pushbuffer-space wait blocks on). */
static void gpu_notify(uint32_t param)
{
    xbox_Nv2aSoftwareMethod(param);
}
extern void xbox_FramebufferWindowPresentPixels(const uint32_t *xrgb, uint32_t width,
                                                uint32_t height);
extern void xbox_FramebufferWindowFrameStats(uint32_t draws);
extern void xbox_FramebufferWindowExternalPresent(int on);
extern void *xbox_FramebufferWindowHwnd(void);
extern void *xbox_FramebufferWindowSdl(void);
extern void xbox_FramebufferWindowGpuPresent(int on);
extern int xbox_FramebufferWindowClientSize(uint32_t *width, uint32_t *height);

#define GPU_VRAM_VA      0x80000000u        /* the contiguous window = physical view */
#define GPU_VRAM_SIZE    (64u << 20)
#define GPU_APERTURE_VA  0xFD000000u        /* NV2A registers, plain memory here */
#define GPU_PFIFO_OFF    0x00002000u
#define GPU_PRAMIN_OFF   0x00700000u
#define GPU_PRAMIN_SIZE  (1u << 20)
#define GPU_DMA_PUT_OFF  0x00800040u        /* channel 0's DMA_PUT (USER block) */
#define GPU_FENCE_OFF    0x00400B10u        /* NV_PGRAPH_PATT_COLOR0 */

/* Each GPU frame goes to the framebuffer window (RECOMP_FB_WINDOW), as the software
 * executor's flip does with guest memory, unless the renderer drew it into the window
 * itself (xrgb = NULL, plan 2.6b); the title bar's stats either way. */
static void gpu_present(const uint32_t *xrgb, unsigned width, unsigned height, unsigned draws)
{
    if (xrgb)
        xbox_FramebufferWindowPresentPixels(xrgb, width, height);
    xbox_FramebufferWindowFrameStats(draws);
}

/* Returns true when the GPU renderer is up. */
static bool gpu_init_from_env(void)
{
    const char *e = getenv("RECOMP_PB_GPU");
    if (!e || strcmp(e, "1") != 0)
        return false;
    const char *renderer = getenv("RECOMP_PB_GPU_RENDERER");
    int opengl = !(renderer && strcmp(renderer, "null") == 0);
    const char *vblank = getenv("RECOMP_PB_GPU_VBLANK");
    if (vblank && vblank[0])
        mksm_gpu_set_vblank_hz(atof(vblank));
    uint8_t *base = (uint8_t *)xbox_GetMemoryOffset();
    MksmGpuMemory mem = {
        .vram = base + GPU_VRAM_VA,
        .vram_size = GPU_VRAM_SIZE,
        .ramin = base + GPU_APERTURE_VA + GPU_PRAMIN_OFF,
        .ramin_size = GPU_PRAMIN_SIZE,
        .pfifo = (const volatile uint32_t *)(base + GPU_APERTURE_VA + GPU_PFIFO_OFF),
        .dma_put = (const volatile uint32_t *)(base + GPU_APERTURE_VA + GPU_DMA_PUT_OFF),
        .pgraph_fence = (volatile uint32_t *)(base + GPU_APERTURE_VA + GPU_FENCE_OFF),
    };
    /* RECOMP_PB_GPU_DIRTY=always: every texture/vertex range reads as dirty on every use
     * (the pre-2.H.8 behaviour, for A/B runs and as a fallback). =kick (the per-kick answer
     * without the texture-page hash) is read in mksm_gpu_init. */
    const char *dirty = getenv("RECOMP_PB_GPU_DIRTY");
    if (dirty && strcmp(dirty, "always") == 0)
        mem.dma_put = NULL;
    if (mksm_gpu_init(&mem, opengl) != 0) {
        fprintf(stderr, "[BOOT] RECOMP_PB_GPU: PGRAPH init failed, software executor\n");
        return false;
    }
    mksm_gpu_set_present(gpu_present);
    {
        const MksmGpuWindow window = {
#ifdef MKSM_GL_WGL
            .window = xbox_FramebufferWindowHwnd,   /* GDI window, WGL contexts */
#else
            .window = xbox_FramebufferWindowSdl,    /* SDL window, SDL GL contexts (B9a2) */
#endif
            .gpu_present = xbox_FramebufferWindowGpuPresent,
            .client_size = xbox_FramebufferWindowClientSize,
        };
        mksm_gpu_set_window(&window);
    }
    /* The window present draws into the window itself: the SDL window (B9a1) then makes
     * no renderer of its own. Same test as mksm_gpu.c's present mode; before the window
     * opens (the toolkit opens it at the first display-mode set). */
    {
        const char *pm = getenv("RECOMP_PB_GPU_PRESENT");
        if (opengl && (!pm || !*pm || strcmp(pm, "window") == 0)) {
            xbox_FramebufferWindowExternalPresent(1);
#ifdef MKSM_HAVE_MENU
            mksm_menu_install();    /* the settings menu over the window present (B7a1) */
#endif
        }
    }
    mksm_gpu_set_notify(gpu_notify);
    nv2a_pb_set_method_hook(mksm_gpu_method);
    fprintf(stderr, "[BOOT] RECOMP_PB_GPU: pushbuffer -> xemu PGRAPH\n");
    return true;
}
#else
static bool gpu_init_from_env(void)
{
    if (getenv("RECOMP_PB_GPU"))
        fprintf(stderr, "[BOOT] RECOMP_PB_GPU ignored: built without mksm_gpu\n");
    return false;
}
#endif

/* D3D's kickoff (every pushbuffer submit) sets the PFB write-back-cache flush bit and
 * spins until the GPU clears it, then writes DMA_PUT. The toolkit's ack thread clears it,
 * but only once per pass, after executing the whole previous segment (and, with the GPU
 * renderer, the present and the VBlank wait), so the title and the GPU run in lockstep:
 * the title spends ~93% of its time in that spin in the level (plan 2.H.8). Host memory is
 * coherent, so there is nothing to flush, and this thread acks the bit within ~0.25 ms.
 * Running ahead, the title then waits on two things only the GPU path provides: D3D's
 * pushbuffer fence (PGRAPH PATT_COLOR0, mirrored by mksm_gpu) and software method 5
 * (gpu_notify → xbox_Nv2aSoftwareMethod), and a DMA_GET that moves during the ring walk
 * (toolkit patch 0014; with GET set only after each segment, D3D took a stale GET as "the GPU
 * passed the ring's end" and overwrote commands not yet executed). On by default on the GPU
 * path; RECOMP_WBC_ACK=0 turns it off, =1 forces it on (the software executor has no fence
 * mirror or software methods and hangs with it). */
#define WBC_FLUSH_VA   (0xFD000000u + 0x100410u)    /* NV_PFB_WBC */
#define WBC_FLUSH_BUSY 0x00010000u

static DWORD WINAPI wbc_ack_thread(LPVOID param)
{
    volatile LONG *wbc = (volatile LONG *)param;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    for (;;) {
        if (*wbc & WBC_FLUSH_BUSY)
            InterlockedAnd(wbc, ~(LONG)WBC_FLUSH_BUSY);
        LARGE_INTEGER due = { .QuadPart = -2500 };  /* 250 us, relative */
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(timer, INFINITE);
        else
            Sleep(1);
    }
    return 0;
}

static void wbc_ack_start(bool gpu)
{
    const char *e = getenv("RECOMP_WBC_ACK");
    if (e ? strcmp(e, "1") != 0 : !gpu)
        return;
    void *wbc = (uint8_t *)xbox_GetMemoryOffset() + WBC_FLUSH_VA;
    HANDLE th = CreateThread(NULL, 0, wbc_ack_thread, wbc, 0, NULL);
    if (th) {
        xbox_name_thread(th, "wbc ack");
        SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(th);
    }
}

/* D3D's BlockUntilVerticalBlank resets a notification event and waits for the VBlank DPC to
 * set it; the runtime delivers no NV2A interrupts, so the title's VBlank-paced worker threads
 * spun on a wait that failed at once (~1.3 cores, plan 2.7a3). This thread stands in for the
 * PCRTC: on a fixed 59.94 Hz grid (RECOMP_PB_GPU_VBLANK, if > 0) it has the toolkit set that
 * event (xbox_Nv2aVBlank). The wait only blocks if the event is not in
 * RECOMP_TITLE_KEVENTS_SKIP: listing it there (0x003F29CC) is the old spin, for A/B runs.
 * The GPU path paces its flips on the same ticks (mksm_gpu_vblank_tick). */
extern int xbox_Nv2aVBlank(void);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);

/* D3D's VBlank DPC also calls the title's VBlank callback (D3DDevice_SetVerticalBlankCallback,
 * kept at miniport + 0x190) with a D3DVBLANKDATA. xbox_Nv2aVBlank only sets the event, so the
 * callback never ran: its VBlank counters stayed 0, and the loading screen, which draws its
 * progress bar and presents only when the counter moves, showed no bar (plan 2.H.5a). The
 * game's callback only counts, so it's mirrored here rather than run as guest code on this
 * host thread: field parity at 0x0063898C, VBlanks at 0x00637B20, and at 0x00638990 unless
 * 0x006C92EC is set. Only while the registered callback is that one; any other is logged once.
 * RECOMP_TITLE_VBLANK_CB=0 turns it off (A/B); RECOMP_VBLANK_STATS=1 prints the counters
 * once a second. */
#define TITLE_VBLANK_CB_VA      0x00119030u
#define TITLE_VBLANK_FIELD_VA   0x0063898Cu
#define TITLE_VBLANK_COUNT_VA   0x00637B20u
#define TITLE_VBLANK_COUNT2_VA  0x00638990u
#define TITLE_VBLANK_HOLD_VA    0x006C92ECu
#define NV2A_IRQ_VECTOR         3u

static void title_vblank_callback(uint32_t vblank)
{
    static int on = -1, stats = -1;
    static uint32_t other_cb;
    uint8_t *base = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t kint, miniport, cb;

    if (on < 0) {
        const char *e = getenv("RECOMP_TITLE_VBLANK_CB");
        const char *s = getenv("RECOMP_VBLANK_STATS");
        on = !(e && e[0] == '0');
        stats = s && s[0] && s[0] != '0';
    }
    kint = xbox_GetConnectedInterrupt(NV2A_IRQ_VECTOR);
    miniport = kint ? *(volatile uint32_t *)(base + kint + 4) : 0;
    cb = miniport ? *(volatile uint32_t *)(base + miniport + 0x190) : 0;

    if (on && cb == TITLE_VBLANK_CB_VA) {
        *(volatile uint8_t *)(base + TITLE_VBLANK_FIELD_VA) = (uint8_t)(vblank & 1);
        (*(volatile uint32_t *)(base + TITLE_VBLANK_COUNT_VA))++;
        if (!*(volatile uint32_t *)(base + TITLE_VBLANK_HOLD_VA))
            (*(volatile uint32_t *)(base + TITLE_VBLANK_COUNT2_VA))++;
    } else if (cb && cb != TITLE_VBLANK_CB_VA && cb != other_cb) {
        other_cb = cb;
        fprintf(stderr, "[VBLANK] unknown title callback 0x%08X, not run\n", cb);
    }
    if (stats && vblank % 60 == 0)
        fprintf(stderr, "[VBLANK] tick %u cb 0x%08X%s count 0x%08X count2 0x%08X hold %u\n",
                vblank, cb, on ? "" : " (off)",
                *(volatile uint32_t *)(base + TITLE_VBLANK_COUNT_VA),
                *(volatile uint32_t *)(base + TITLE_VBLANK_COUNT2_VA),
                *(volatile uint32_t *)(base + TITLE_VBLANK_HOLD_VA));
}

static DWORD WINAPI vblank_thread(LPVOID param)
{
    double hz = *(double *)param;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const double period = (double)freq.QuadPart / hz;
    double next = (double)now.QuadPart + period;
    uint32_t vblank = 0;
    for (;;) {
        QueryPerformanceCounter(&now);
        double ahead = next - (double)now.QuadPart;
        if (ahead > 0) {
            LARGE_INTEGER due = { .QuadPart = -(LONGLONG)(ahead * 1e7 / (double)freq.QuadPart) };
            if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                WaitForSingleObject(timer, INFINITE);
            else
                Sleep((DWORD)(ahead * 1000.0 / (double)freq.QuadPart));
        } else if (-ahead > 4 * period) {
            next = (double)now.QuadPart;           /* far behind (a stall): restart the grid */
        }
        next += period;
        /* Callback before the event, so a thread woken by the event sees the new count. */
        title_vblank_callback(++vblank);
        xbox_Nv2aVBlank();
#ifdef MKSM_HAVE_GPU
        mksm_gpu_vblank_tick();                    /* flips pace on the same ticks */
#endif
    }
    return 0;
}

static void vblank_start(void)
{
    static double hz = 60000.0 / 1001.0;
    const char *v = getenv("RECOMP_PB_GPU_VBLANK");
    if (v && atof(v) > 0)
        hz = atof(v);
    HANDLE th = CreateThread(NULL, 0, vblank_thread, &hz, 0, NULL);
    if (th) {
        xbox_name_thread(th, "vblank");
        SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(th);
    }
}

extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
#ifdef _WIN32
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);
#endif

/* The emulated USB host controllers (src/usb/ohci.h), where the game's
 * statically linked XAPI finds its gamepads. Nothing in the toolkit calls
 * these; the title brings the model up and routes its register faults here.
 * Both do nothing unless RECOMP_USB is set. */
extern void xbox_OhciInit(void);
/* The host pads (src/input): with the SDL window, SDL's game controllers (plan B9b1),
 * brought up here on the main thread before other threads use SDL. */
extern void xbox_InputInit(void);
extern int  xbox_OhciOwnsAddress(uint32_t xbox_va);
extern int  xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);

/* The trapped span, matching what MemoryLayoutInit unmaps: the APU's own
 * 512 KB, not the whole MCPX aperture. AC'97 above it stays plain memory,
 * which is what the codec-ready bit needs. */
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u

#ifdef _WIN32
static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* APU registers -> the emulated APU.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        {
            uint32_t xbox_va =
                (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

            if (xbox_va >= APU_TRAP_BASE && xbox_va < APU_TRAP_END
                    && apu_hook_handle_mmio(
                           ep->ContextRecord, fault_addr, xbox_va,
                           ep->ExceptionRecord->ExceptionInformation[0]
                               ? 1 : 0)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            /* OHCI registers -> the emulated USB controllers (gamepads). */
            if (xbox_OhciOwnsAddress(xbox_va)
                    && xbox_OhciHandleMmio(ep->ContextRecord, xbox_va)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}
#else
/* POSIX: the same report from a SIGSEGV/SIGBUS handler. Trapped hardware registers (APU,
 * AC'97, OHCI) go to the toolkit first (xbox_PosixMmioFault; it has a decoder on arm64 only
 * so far, plan H.5c2b). stdio in a signal handler, as the toolkit's traps do: the process is
 * about to die. */
static void posix_fault(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    uintptr_t fault_addr = (uintptr_t)si->si_addr;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    int write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;

    if (xbox_PosixMmioFault(ucv, fault_addr))
        return;

    fprintf(stderr, "[CRASH] %s at RIP=0x%llX, fault addr=0x%llX (%s)\n",
        sig == SIGBUS ? "Bus error" : "Access violation",
        (unsigned long long)rip, (unsigned long long)fault_addr, write ? "write" : "read");
    fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
        g_eax, g_ecx, g_edx, g_esp);
    fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
        g_ebx, g_esi, g_edi);
    fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
        (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
    print_guest_context((void *)rip);
    fflush(stderr);
    /* Back to the default action: returning re-runs the access, which now kills the
     * process (core dump, the signal's exit status). */
    signal(sig, SIG_DFL);
}

static void posix_fault_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = posix_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}
#endif

/* ── Game directory ────────────────────────────────────────── */

typedef enum GameDirFrom {
    GAME_DIR_ARG,       /* argv[1] */
    GAME_DIR_ENV,       /* MKSM_GAME_DIR set by the user */
    GAME_DIR_FILE,      /* settings.ini's game_dir= */
    GAME_DIR_DEFAULT,   /* "game" in the working directory */
} GameDirFrom;

static void set_game_dir(const char *dir)
{
    size_t len;

    full_path(dir, g_game_dir);

    /* Drop trailing separators, but keep a bare drive root like "D:\" (or "/"). */
    len = strlen(g_game_dir);
    while (len > (DIRSEP[0] == '/' ? 1 : 3)
            && (g_game_dir[len - 1] == '\\' || g_game_dir[len - 1] == '/'))
        g_game_dir[--len] = '\0';

    snprintf(g_xbe_path, MAX_PATH, "%s" DIRSEP "default.xbe", g_game_dir);
}

/* After mksm_settings_load: its game_dir= arrives as MKSM_GAME_DIR. */
static GameDirFrom resolve_game_dir(void)
{
    const char *env = getenv("MKSM_GAME_DIR");

    if (mksm_argc > 1 && mksm_argv[1][0]) {
        set_game_dir(mksm_argv[1]);
        return GAME_DIR_ARG;
    }
    if (env && env[0]) {
        set_game_dir(env);
        return mksm_settings_env_from_file("MKSM_GAME_DIR") ? GAME_DIR_FILE : GAME_DIR_ENV;
    }
    set_game_dir(DEFAULT_GAME_DIR);
    if (!path_exists(g_xbe_path)) {
        /* game/ beside the exe (where the setup extracts to), when started from elsewhere. */
        char exe_game[MAX_PATH];
        if (exe_dir(exe_game)
                && strlen(exe_game) + sizeof(DIRSEP DEFAULT_GAME_DIR) <= MAX_PATH) {
            strcat(exe_game, DIRSEP DEFAULT_GAME_DIR);
            snprintf(g_xbe_path, MAX_PATH, "%s" DIRSEP "default.xbe", exe_game);
            if (path_exists(g_xbe_path))
                set_game_dir(exe_game);
            else
                set_game_dir(DEFAULT_GAME_DIR);   /* both missing: report the working dir's */
        }
    }
    return GAME_DIR_DEFAULT;
}

/* ── Base and save directories ─────────────────────────────── */

static void trim_dir(char *dir)
{
    size_t len = strlen(dir);
    while (len > (DIRSEP[0] == '/' ? 1 : 3) && (dir[len - 1] == '\\' || dir[len - 1] == '/'))
        dir[--len] = '\0';
}

/* The per-user data folder: %LOCALAPPDATA% on Windows, $XDG_DATA_HOME or ~/.local/share on
 * POSIX. false if unknown. */
static bool user_data_dir(char *out)
{
#ifdef _WIN32
    return SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, out));
#else
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0] == '/')
        snprintf(out, MAX_PATH, "%s", xdg);
    else if (home && home[0])
        snprintf(out, MAX_PATH, "%s/.local/share", home);
    else
        return false;
    return true;
#endif
}

static void resolve_base_dir(void)
{
    char base[MAX_PATH];

    if (user_data_dir(base)) {
        snprintf(g_base_dir, MAX_PATH, "%s" DIRSEP SAVE_DIR_NAME, base);
    } else {
        /* Next to the exe (the game dir isn't known yet: settings.ini may name it). */
        if (!exe_dir(base))
            snprintf(base, MAX_PATH, ".");
        snprintf(g_base_dir, MAX_PATH, "%s" DIRSEP SAVE_DIR_NAME, base);
    }
    trim_dir(g_base_dir);
}

/* After mksm_settings_load: its save_dir= arrives as MKSM_SAVE_DIR. */
static void resolve_save_dir(void)
{
    const char *env = getenv("MKSM_SAVE_DIR");

    if (env && env[0])
        full_path(env, g_save_dir);
    else
        snprintf(g_save_dir, MAX_PATH, "%s" DIRSEP "saves", g_base_dir);
    trim_dir(g_save_dir);
}

/* Saves from older builds, moved into the save dir once so a player keeps
 * them: <base>\UDATA/TDATA (builds before B7b1 kept them next to the disk
 * images; not when MKSM_SAVE_DIR comes from the environment: a dev run's
 * folder shouldn't take the player's saves), then <game dir>\UDATA/TDATA
 * (before toolkit v0.14's UDATA/TDATA rules). Only into a save dir that has
 * none yet, and not with RECOMP_SAVE_DIR set by hand (it sends them
 * elsewhere). A move across volumes fails for a directory: base saves then
 * stay where they are (logged); game-dir saves too, and the save dir isn't
 * handed to the toolkit, whose own fallback keeps using them there. */
static bool move_saves(const char *from_dir, const char *what)
{
    static const char *const names[] = { "UDATA", "TDATA" };
    bool ok = true;

    for (int i = 0; i < 2; i++) {
        char from[MAX_PATH], to[MAX_PATH];
        snprintf(from, MAX_PATH, "%s" DIRSEP "%s", from_dir, names[i]);
        snprintf(to, MAX_PATH, "%s" DIRSEP "%s", g_save_dir, names[i]);
        if (!path_is_dir(from))
            continue;
        if (path_exists(to))
            continue;
        if (move_path(from, to)) {
            fprintf(stderr, "[SAVE] moved %s to %s\n", from, to);
        } else {
#ifdef _WIN32
            fprintf(stderr, "[SAVE] could not move %s (%s) to %s (error %lu)\n",
                    from, what, to, GetLastError());
#else
            fprintf(stderr, "[SAVE] could not move %s (%s) to %s (%s)\n",
                    from, what, to, strerror(errno));
#endif
            ok = false;
        }
    }
    return ok;
}

static void migrate_legacy_saves(void)
{
    const char *ov = getenv("RECOMP_SAVE_DIR");

    if (ov && ov[0]) {
        g_save_dir_toolkit = false;
        snprintf(g_save_dir, MAX_PATH, "%s", ov);   /* for the log line */
        return;
    }
    make_dirs(g_save_dir);  /* absolute, any depth */
#ifdef _WIN32
    if (_stricmp(g_save_dir, g_base_dir) != 0
#else
    if (strcmp(g_save_dir, g_base_dir) != 0
#endif
            && (!getenv("MKSM_SAVE_DIR") || mksm_settings_env_from_file("MKSM_SAVE_DIR")))
        move_saves(g_base_dir, "old save location");
    if (!move_saves(g_game_dir, "saves in the game folder")) {
        g_save_dir_toolkit = false;
        fprintf(stderr, "[SAVE] the game folder's saves stay in use there\n");
    }
    if (g_save_dir_toolkit) {
        env_put("RECOMP_SAVE_DIR", g_save_dir);
        mksm_settings_derived_env("RECOMP_SAVE_DIR");  /* worked out again after a restart */
    }
}

/* ── Built-in run defaults (plan B10a) ─────────────────────── */

/* What the run scripts set for the game to run, as the exe's own defaults, so a
 * double-clicked exe runs as tools/run.sh does. Applied after the settings file
 * (environment > settings.ini > this table), only where the variable isn't set at
 * all: an empty value counts as set (RECOMP_APU_EP_ALIVE= / RECOMP_PB_WALK= are the
 * A/B "off"). presence: the toolkit only checks that the variable exists, so 0 or
 * empty must remove it. MKSM_DEFAULTS=0 skips the table (the bare toolkit, e.g. for
 * a capture replay). */
typedef struct EnvDefault {
    const char *name;
    const char *value;
    bool presence;
} EnvDefault;

static const EnvDefault k_env_defaults[] = {
    { "RECOMP_PB_EXEC",         "1",          true  },  /* D3D init waits for the GPU */
    { "RECOMP_PB_GPU",          "1",          false },  /* xemu's PGRAPH draws */
    { "RECOMP_FB_WINDOW",       "1",          true  },  /* the game window */
    { "RECOMP_USB",             "1",          true  },  /* emulated OHCI: the pads */
    { "RECOMP_KEYBOARD",        "1",          false },  /* keyboard on pad 1 */
    { "RECOMP_AC97_READY",      "1",          true  },  /* codec ready + emulated APU */
    { "RECOMP_APU_DSP_ACK",     "auto",       false },  /* (the toolkit's default too) */
    { "RECOMP_APU_EP_ALIVE",    "0xFE85A018", false },  /* plan 2.7a6 */
    { "RECOMP_HEAP_RECLAIM",    "1",          true  },  /* contiguous arena reuse */
    { "RECOMP_TITLE_KEVENTS",   "1",          false },  /* host events for title events */
    { "RECOMP_PB_WALK",         "dma",        false },  /* plan H.5a */
    { "RECOMP_AUDIO_BUF_COUNT", "8",          false },  /* 85 ms, plan H.5a */
};

#ifndef _WIN32
/* POSIX has one environment, so a presence variable turned off must be unset for the toolkit
 * to see it off, and a settings relaunch (execv) would then find it unset and turn it on
 * again. The names turned off travel in MKSM_ENV_OFF (comma-separated) instead, as Windows'
 * process block keeps their 0. */
static bool env_off_listed(const char *name)
{
    const char *p = getenv("MKSM_ENV_OFF");
    size_t len = strlen(name);
    while (p && *p) {
        if (strncmp(p, name, len) == 0 && (p[len] == ',' || p[len] == '\0'))
            return true;
        p = strchr(p, ',');
        if (p)
            p++;
    }
    return false;
}

static void env_off_add(const char *name)
{
    const char *list = getenv("MKSM_ENV_OFF");
    char buf[1024];
    if (env_off_listed(name))
        return;
    if (list && *list)
        snprintf(buf, sizeof(buf), "%s,%s", list, name);
    else
        snprintf(buf, sizeof(buf), "%s", name);
    setenv("MKSM_ENV_OFF", buf, 1);
}

/* 0 = unset, 1 = set (value in buf, may be empty; "0" for a name in MKSM_ENV_OFF). */
static int env_state(const char *name, char *buf, size_t cap)
{
    const char *v = getenv(name);
    if (!v) {
        int off = env_off_listed(name);
        snprintf(buf, cap, "%s", off ? "0" : "");
        return off;
    }
    if (strlen(v) >= cap)
        buf[0] = '\0';          /* too long for us: only "set" matters then */
    else
        snprintf(buf, cap, "%s", v);
    return 1;
}
#else
/* 0 = unset, 1 = set (value in buf, may be empty). Reads the process environment
 * block, which tells an empty variable from a missing one. */
static int env_state(const char *name, char *buf, DWORD cap)
{
    DWORD n;
    SetLastError(0);
    n = GetEnvironmentVariableA(name, buf, cap);
    if (n == 0) {
        buf[0] = '\0';
        return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? 0 : 1;
    }
    if (n >= cap)
        buf[0] = '\0';          /* too long for us: only "set" matters then */
    return 1;
}
#endif

static void apply_env_defaults(void)
{
    const char *skip = getenv("MKSM_DEFAULTS");
    char val[64];

    if (skip && strcmp(skip, "0") == 0) {
        fprintf(stderr, "[ENV] built-in defaults skipped (MKSM_DEFAULTS=0)\n");
        return;
    }
    for (size_t i = 0; i < sizeof(k_env_defaults) / sizeof(k_env_defaults[0]); i++) {
        const EnvDefault *d = &k_env_defaults[i];
        if (!env_state(d->name, val, sizeof(val))) {
            env_put(d->name, d->value);
            /* A relaunch (settings menu) works it out again, like a file value. */
            mksm_settings_derived_env(d->name);
            fprintf(stderr, "[ENV] %s=%s (default)\n", d->name, d->value);
        } else if (d->presence && (!val[0] || strcmp(val, "0") == 0)) {
            /* getenv (the CRT's copy) no longer has it, so the toolkit sees off; the
             * process block keeps 0, so a relaunched child is off too. */
#ifdef _WIN32
            _putenv_s(d->name, "");
            SetEnvironmentVariableA(d->name, "0");
#else
            unsetenv(d->name);
            env_off_add(d->name);
#endif
            fprintf(stderr, "[ENV] %s off\n", d->name);
        }
    }
}

/* ── Player log (plan B10a) ────────────────────────────────── */

/* Started from Explorer, a GUI-subsystem exe has nowhere to print: stdout/stderr
 * then go to %LOCALAPPDATA%\mksm_recomp\last_run.log (the one before kept as
 * last_run.old.log). A pipe or a file (the run scripts, smoke_test.py) is left
 * alone. A settings relaunch inherits the handle and writes on into the same file. */
#ifdef _WIN32
static bool std_redirected(DWORD which)
{
    HANDLE h = GetStdHandle(which);
    DWORD type;
    if (!h || h == INVALID_HANDLE_VALUE)
        return false;
    type = GetFileType(h);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

static void attach_stream(FILE *s, int fd)
{
    FILE *tmp;
    /* No handle at start: the CRT stream has no descriptor; give it one to replace. */
    if (_fileno(s) < 0 && freopen_s(&tmp, "NUL", "w", s) != 0)
        return;
    _dup2(fd, _fileno(s));
}

static void player_log_open(void)
{
    char dir[MAX_PATH], path[MAX_PATH], old[MAX_PATH];
    HANDLE h;
    int fd;

    if (std_redirected(STD_OUTPUT_HANDLE) || std_redirected(STD_ERROR_HANDLE))
        return;
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, dir)))
        return;
    snprintf(path, MAX_PATH, "%s\\" SAVE_DIR_NAME, dir);
    CreateDirectoryA(path, NULL);
    snprintf(old, MAX_PATH, "%s\\last_run.old.log", path);
    snprintf(dir, MAX_PATH, "%s", path);
    snprintf(path, MAX_PATH, "%s\\last_run.log", dir);
    MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING);  /* none yet, or in use: fine */
    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_TEXT);
    if (fd < 0) {
        CloseHandle(h);
        return;
    }
    SetStdHandle(STD_OUTPUT_HANDLE, h);   /* GetStdHandle users, and a settings relaunch */
    SetStdHandle(STD_ERROR_HANDLE, h);
    attach_stream(stdout, fd);
    attach_stream(stderr, fd);
    /* fd stays open for the process lifetime: it owns h. */
}
#else
/* POSIX: stdout/stderr stay where the launcher sent them (a terminal, Steam's log). */
static void player_log_open(void) {}
#endif

/* ── WinMain ───────────────────────────────────────────────── */

static int mksm_main(void)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    /* Double-clicked: the output goes to <base>\last_run.log (B10a). Before setvbuf:
     * freopen resets a stream's buffering. */
    player_log_open();
    /* Unbuffered output for immediate visibility during debugging (and so a crash
     * leaves the whole log on disk) */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== MK Shaolin Monks - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
#ifdef _WIN32
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
#else
    posix_fault_install();
#endif
    prof_start_from_env();

    /* Step 1: Load XBE */
    /* The settings file before anything reads the variables it sets (plan B7b1), the game
     * dir included (game_dir=, B10b2). */
    resolve_base_dir();
    mksm_settings_load(g_base_dir);
    /* The run scripts' defaults, where neither the environment nor the file set them (B10a). */
    apply_env_defaults();
    {
        GameDirFrom from = resolve_game_dir();
        /* Nothing the user named: ask for the disc once (B10b2). A folder the user gave by
         * argument or variable is theirs to fix (the message below). */
        if ((from == GAME_DIR_FILE || from == GAME_DIR_DEFAULT)
                && !path_exists(g_xbe_path)) {
            char picked[MAX_PATH];
            fprintf(stderr, "[SETUP] no default.xbe in %s: first-launch setup\n", g_game_dir);
            if (!mksm_setup_run(g_base_dir, picked, sizeof(picked)))
                return 1;
            set_game_dir(picked);
        }
    }
    printf("Game dir: %s\n", g_game_dir);
    if (!load_xbe(g_xbe_path, &xbe_data, &xbe_size)) {
        fprintf(stderr, "Failed to load %s\n", g_xbe_path);
        MessageBoxA(NULL, "Failed to load default.xbe.\n"
                    "Pass the extracted game folder as the first argument,\n"
                    "set MKSM_GAME_DIR, or put it in a 'game' subdirectory.",
                    "Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);
    mksm_setup_log_xbe(g_xbe_path);     /* a different build is logged, not refused */

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* RECOMP_PB_GPU=1: before the replay below, so a capture can replay through it too. */
    wbc_ack_start(gpu_init_from_env());
    vblank_start();

    /* Replay a captured frame instead of booting the game (RECOMP_PB_REPLAY).
     *
     * The normal memory setup has already run (sections mapped, 64 MiB RAM +
     * mirrors + contiguous window + NV2A aperture). The capture restores the
     * two regions the frame's draws read, replays prelude + frame through the
     * software executor, and dumps a BMP -- then exits without starting the
     * game. This is the fast loop for the GPU renderer work (2.H.3+). */
    {
        const char *replay = getenv("RECOMP_PB_REPLAY");
        if (replay && replay[0]) {
            extern int nv2a_pb_capture_replay(const char *path);
            int rc = nv2a_pb_capture_replay(replay);
            xbox_MemoryLayoutShutdown();
            free(xbe_data);
            return rc;
        }
    }

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Bring up the USB host controllers (RECOMP_USB). Needs the guest memory
     * mapped: it makes their register pages fault so the VEH can answer. */
    if (getenv("RECOMP_USB"))
        xbox_InputInit();
    xbox_OhciInit();

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        resolve_save_dir();
        migrate_legacy_saves();
        fprintf(stderr, "[BOOT] save dir: %s (disk images: %s)\n",
                g_save_dir_toolkit || getenv("RECOMP_SAVE_DIR") ? g_save_dir : g_game_dir,
                g_base_dir);
        xbox_path_init(g_game_dir, g_base_dir);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;
    return mksm_main();
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
#else
int main(int argc, char **argv)
{
    mksm_argc = argc;
    mksm_argv = argv;
    return mksm_main();
}
#endif
