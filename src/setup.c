/* First-launch setup (plan B10b2); see setup.h. */
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "settings.h"
#include "setup.h"
#include "xiso.h"

/* Themed controls for the progress window and the message boxes. */
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                        "language='*'\"")

#define TITLE L"Mortal Kombat: Shaolin Monks"
#define GB(x) ((double)(x) / 1e9)

static int g_auto;      /* MKSM_SETUP_PICK: no dialogs, log only */

/* ── Text helpers ──────────────────────────────────────────── */

static void log_w(const wchar_t *msg)
{
    char out[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, out, sizeof(out), NULL, NULL);
    if (n <= 0)
        snprintf(out, sizeof(out), "(message not printable)");
    for (char *p = out; *p; p++)
        if (*p == '\n')
            *p = ' ';
    fprintf(stderr, "[SETUP] %s\n", out);
}

/* Logged, and shown unless MKSM_SETUP_PICK is set. */
static void say(UINT icon, const wchar_t *fmt, ...)
{
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, sizeof(msg) / sizeof(msg[0]), _TRUNCATE, fmt, ap);
    va_end(ap);
    log_w(msg);
    if (!g_auto)
        MessageBoxW(NULL, msg, TITLE, MB_OK | icon);
}

static void to_wide(const char *s, wchar_t *out, int cap)
{
    if (!MultiByteToWideChar(CP_ACP, 0, s, -1, out, cap))
        out[0] = L'\0';
}

/* 0 when the path doesn't fit the ANSI code page (main.c and the toolkit take char paths). */
static int to_ansi(const wchar_t *s, char *out, size_t cap)
{
    BOOL lossy = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, s, -1, out, (int)cap, NULL, &lossy);
    return n > 0 && !lossy;
}

/* Deletes a folder tree for good (our own game.partial only). */
static void remove_tree(const wchar_t *dir)
{
    wchar_t from[MAX_PATH + 1];
    SHFILEOPSTRUCTW op = { 0 };
    size_t n = wcslen(dir);
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES || n >= MAX_PATH)
        return;
    wmemcpy(from, dir, n);
    from[n] = from[n + 1] = L'\0';      /* a double-NUL list */
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

static void wrong_version(const uint8_t sha256[32])
{
    char hex[65];
    xiso_hex(sha256, 32, hex);
    say(MB_ICONWARNING,
        L"This is a different version of the game.\n\n"
        L"This port needs the USA/Europe disc of Mortal Kombat: Shaolin Monks for the original "
        L"Xbox (its default.xbe).\n\n(default.xbe SHA-256 %hs)", hex);
}

/* ── An extracted folder ───────────────────────────────────── */

/* 1 = <dir>\default.xbe is the known build (dir copied to out). quiet: no messages. */
static int check_folder(const wchar_t *dir, char *out, size_t cap, int quiet)
{
    wchar_t xbe[MAX_PATH];
    uint8_t h[32];
    int rc;

    if (_snwprintf_s(xbe, MAX_PATH, _TRUNCATE, L"%ls\\default.xbe", dir) < 0)
        return 0;
    rc = xiso_hash_file(xbe, L"SHA256", h, sizeof(h));
    if (rc) {
        if (!quiet)
            say(MB_ICONERROR, L"No game files in\n%ls\n\n(default.xbe: %hs)", dir,
                xiso_strerror(rc));
        return 0;
    }
    if (!mksm_xbe_is_known(h)) {
        if (!quiet)
            wrong_version(h);
        return 0;
    }
    if (!to_ansi(dir, out, cap)) {
        if (!quiet)
            say(MB_ICONERROR, L"The folder\n%ls\nhas characters in its path this port can't use "
                L"yet. Move it to a folder with a plain name (e.g. C:\\Games\\mksm).", dir);
        return 0;
    }
    return 1;
}

/* ── Extraction with a progress window ─────────────────────── */

#define WM_JOB_PROGRESS (WM_APP + 1)
#define WM_JOB_DONE     (WM_APP + 2)
#define ID_LABEL        101
#define ID_BAR          102

typedef struct Job {
    Xiso *x;
    const wchar_t *out_dir;
    HWND hwnd;
    volatile LONG cancel;
    CRITICAL_SECTION lock;      /* done/total/path */
    uint64_t done, total;
    char path[96];
    int rc;
    unsigned long os_error;
} Job;

/* Worker thread: hands the numbers to the window, which repaints from its own thread. */
static int job_progress(void *user, uint64_t done, uint64_t total, const char *path)
{
    Job *j = user;
    EnterCriticalSection(&j->lock);
    j->done = done;
    j->total = total;
    snprintf(j->path, sizeof(j->path), "%s", path);
    LeaveCriticalSection(&j->lock);
    PostMessageW(j->hwnd, WM_JOB_PROGRESS, 0, 0);
    return j->cancel != 0;
}

static DWORD WINAPI job_thread(LPVOID param)
{
    Job *j = param;
    j->rc = xiso_extract(j->x, j->out_dir, job_progress, j, &j->os_error);
    PostMessageW(j->hwnd, WM_JOB_DONE, 0, 0);
    return 0;
}

static LRESULT CALLBACK progress_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Job *j = (Job *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_JOB_PROGRESS:
        if (j && !j->cancel) {
            wchar_t text[256];
            uint64_t done, total;
            char path[96];
            EnterCriticalSection(&j->lock);
            done = j->done;
            total = j->total;
            memcpy(path, j->path, sizeof(path));
            LeaveCriticalSection(&j->lock);
            _snwprintf_s(text, 256, _TRUNCATE,
                         L"Extracting the game disc (only this once)...\n%.2f of %.2f GB\n%hs",
                         GB(done), GB(total), path);
            SetDlgItemTextW(hwnd, ID_LABEL, text);
            SendDlgItemMessageW(hwnd, ID_BAR, PBM_SETPOS,
                                total ? (WPARAM)(done * 1000 / total) : 0, 0);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) != IDCANCEL)
            break;
        /* fall through */
    case WM_CLOSE:
        /* Stops at the next chunk; the window goes once the worker is done. */
        if (j && !j->cancel) {
            InterlockedExchange(&j->cancel, 1);
            SetDlgItemTextW(hwnd, ID_LABEL, L"Cancelling...");
            EnableWindow(GetDlgItem(hwnd, IDCANCEL), FALSE);
        }
        return 0;
    case WM_JOB_DONE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int run_extract(Xiso *x, const wchar_t *out_dir, unsigned long *os_error)
{
    static const wchar_t cls[] = L"mksm_setup_progress";
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    WNDCLASSW wc = { 0 };
    HINSTANCE inst = GetModuleHandleW(NULL);
    HFONT font = NULL;
    Job j = { x, out_dir };
    HANDLE thread;
    HWND hwnd;
    RECT r, work;
    MSG m;
    int dpi = (int)GetDpiForSystem();
#define S(v) MulDiv((v), dpi, 96)

    InitCommonControlsEx(&icc);
    wc.lpfnWndProc = progress_proc;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);        /* already registered on a second extract: fine */

    r.left = r.top = 0;
    r.right = S(480);
    r.bottom = S(136);
    AdjustWindowRect(&r, style, FALSE);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    hwnd = CreateWindowExW(0, cls, TITLE L" - setup", style,
                           work.left + (work.right - work.left - (r.right - r.left)) / 2,
                           work.top + (work.bottom - work.top - (r.bottom - r.top)) / 2,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!hwnd) {
        /* No window: extract anyway, without progress. */
        fprintf(stderr, "[SETUP] no progress window (error %lu)\n", GetLastError());
        return xiso_extract(x, out_dir, NULL, NULL, os_error);
    }
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        font = CreateFontIndirectW(&ncm.lfMessageFont);
    CreateWindowExW(0, L"STATIC", L"Extracting the game disc (only this once)...",
                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, S(16), S(12), S(448), S(54),
                    hwnd, (HMENU)(INT_PTR)ID_LABEL, inst, NULL);
    CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE, S(16), S(70), S(448), S(18),
                    hwnd, (HMENU)(INT_PTR)ID_BAR, inst, NULL);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    S(364), S(100), S(100), S(26), hwnd, (HMENU)(INT_PTR)IDCANCEL, inst, NULL);
    if (font) {
        SendDlgItemMessageW(hwnd, ID_LABEL, WM_SETFONT, (WPARAM)font, FALSE);
        SendDlgItemMessageW(hwnd, IDCANCEL, WM_SETFONT, (WPARAM)font, FALSE);
    }
    SendDlgItemMessageW(hwnd, ID_BAR, PBM_SETRANGE32, 0, 1000);
#undef S

    InitializeCriticalSection(&j.lock);
    j.hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&j);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    fprintf(stderr, "[SETUP] progress window up\n");
    thread = CreateThread(NULL, 0, job_thread, &j, 0, NULL);
    if (!thread) {
        DestroyWindow(hwnd);
        j.rc = xiso_extract(x, out_dir, NULL, NULL, &j.os_error);
    } else {
        while (GetMessageW(&m, NULL, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    DeleteCriticalSection(&j.lock);
    if (font)
        DeleteObject(font);
    *os_error = j.os_error;
    return j.rc;
}

/* ── A disc image ──────────────────────────────────────────── */

/* 1 = extracted into game (copied to out), 0 = refused (pick again), -1 = stop. */
static int from_image(const wchar_t *path, const wchar_t *game, const wchar_t *partial,
                      char *out, size_t cap)
{
    Xiso *x = NULL;
    XisoEntry e;
    uint8_t h[32];
    uint64_t bytes, need, avail;
    uint32_t files, dirs;
    unsigned long os_error = 0;
    ULONGLONG t0;
    int rc;

    rc = xiso_open_file(path, &x);
    if (rc) {
        say(MB_ICONERROR, L"%ls\n\nisn't an Xbox disc image this port can read (%hs).", path,
            xiso_strerror(rc));
        return 0;
    }
    fprintf(stderr, "[SETUP] Xbox disc image, game partition at 0x%llX\n",
            (unsigned long long)xiso_partition_offset(x));
    rc = xiso_find(x, "default.xbe", &e);
    if (rc == XISO_OK)
        rc = xiso_hash_entry(x, &e, L"SHA256", h, sizeof(h));
    if (rc) {
        say(MB_ICONERROR, L"%ls\n\nhas no readable default.xbe: not an Xbox game disc? (%hs)",
            path, xiso_strerror(rc));
        xiso_close(x);
        return 0;
    }
    if (!mksm_xbe_is_known(h)) {
        wrong_version(h);
        xiso_close(x);
        return 0;
    }
    rc = xiso_totals(x, &bytes, &files, &dirs);
    need = xiso_space_needed(bytes, files, dirs);
    if (rc == XISO_OK && xiso_check_space(partial, need, &avail) == XISO_E_NO_SPACE) {
        say(MB_ICONWARNING, L"Extracting the disc needs %.1f GB free on the drive of\n%ls\n"
            L"(%.1f GB free now). Free some space and start again.", GB(need), game, GB(avail));
        xiso_close(x);
        return -1;
    }
    if (GetFileAttributesW(game) != INVALID_FILE_ATTRIBUTES) {
        say(MB_ICONERROR, L"%ls\nis there already but holds no usable game files. Delete or "
            L"rename it and start again.", game);
        xiso_close(x);
        return -1;
    }

    remove_tree(partial);       /* left by a run that was killed mid-extract */
    log_w(L"extracting the disc image");
    t0 = GetTickCount64();
    rc = run_extract(x, partial, &os_error);
    xiso_close(x);
    if (rc == XISO_E_CANCELLED) {
        log_w(L"extract cancelled");
        remove_tree(partial);
        return -1;
    }
    if (rc) {
        if (rc == XISO_E_WRITE)
            say(MB_ICONERROR, L"Writing the game files to\n%ls\nfailed (Windows error %lu).",
                partial, os_error);
        else
            say(MB_ICONERROR, L"Extracting the disc failed: %hs.", xiso_strerror(rc));
        remove_tree(partial);
        return -1;
    }
    if (!MoveFileExW(partial, game, 0)) {
        say(MB_ICONERROR, L"Couldn't rename\n%ls\nto\n%ls\n(Windows error %lu).", partial, game,
            GetLastError());
        return -1;
    }
    fprintf(stderr, "[SETUP] extracted %.2f GB (%u files) in %.1f s\n", GB(bytes),
            (unsigned)files, (double)(GetTickCount64() - t0) / 1000.0);
    if (!to_ansi(game, out, cap))
        return -1;
    return 1;
}

/* ── The flow ──────────────────────────────────────────────── */

/* 1 = ok, 0 = pick again, -1 = stop. */
static int try_path(const wchar_t *path, const wchar_t *game, const wchar_t *partial,
                    char *out, size_t cap)
{
    DWORD a = GetFileAttributesW(path);
    const wchar_t *ext = wcsrchr(path, L'.');
    const wchar_t *slash = wcsrchr(path, L'\\');

    if (a == INVALID_FILE_ATTRIBUTES) {
        say(MB_ICONERROR, L"Can't open\n%ls", path);
        return 0;
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY)
        return check_folder(path, out, cap, 0);
    if (ext && (!slash || ext > slash) && _wcsicmp(ext, L".xbe") == 0 && slash) {
        /* default.xbe (or any .xbe) picked: its folder. */
        wchar_t dir[MAX_PATH];
        size_t n = (size_t)(slash - path);
        if (n >= MAX_PATH)
            return 0;
        wmemcpy(dir, path, n);
        dir[n] = L'\0';
        if (n == 2)
            wcscat_s(dir, MAX_PATH, L"\\");       /* "D:" → "D:\" */
        return check_folder(dir, out, cap, 0);
    }
    return from_image(path, game, partial, out, cap);
}

/* 1 = picked (path in out), 0 = cancelled, -1 = the dialog didn't open. */
static int pick(wchar_t *out, size_t cap)
{
    static const COMDLG_FILTERSPEC types[] = {
        { L"Xbox disc image or default.xbe", L"*.iso;*.xiso;*.xbe" },
        { L"All files", L"*.*" },
    };
    IFileOpenDialog *d = NULL;
    IShellItem *item = NULL;
    PWSTR p = NULL;
    FILEOPENDIALOGOPTIONS o = 0;
    HRESULT hr;
    int ok = 0;

    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IFileOpenDialog, (void **)&d)))
        return -1;
    IFileOpenDialog_SetTitle(d, L"Pick the game disc image, or default.xbe in the extracted folder");
    IFileOpenDialog_SetFileTypes(d, 2, types);
    IFileOpenDialog_GetOptions(d, &o);
    IFileOpenDialog_SetOptions(d, o | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    hr = IFileOpenDialog_Show(d, NULL);
    if (SUCCEEDED(hr) && SUCCEEDED(IFileOpenDialog_GetResult(d, &item)) &&
        SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &p))) {
        wcsncpy_s(out, cap, p, _TRUNCATE);
        ok = 1;
    } else if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        ok = -1;
    }
    CoTaskMemFree(p);
    if (item)
        IShellItem_Release(item);
    IFileOpenDialog_Release(d);
    return ok;
}

static void save_game_dir(const char *dir)
{
    const MksmSettings *f = mksm_settings_file();
    MksmSettings s;

    if (!f) {
        fprintf(stderr, "[SETUP] game_dir not saved (settings file skipped)\n");
        return;
    }
    s = *f;
    snprintf(s.value[MKSM_SET_GAME_DIR], MKSM_SET_VALUE_MAX, "%s", dir);
    s.present[MKSM_SET_GAME_DIR] = 1;
    if (mksm_settings_save(&s) == 0)
        fprintf(stderr, "[SETUP] game_dir=%s saved\n", dir);
}

int mksm_setup_run(const char *base_dir, char *game_dir, size_t cap)
{
    const char *pick_env = getenv("MKSM_SETUP_PICK");
    const char *sp = mksm_settings_path();
    char dir[MAX_PATH], *slash;
    wchar_t dest[MAX_PATH], game[MAX_PATH], partial[MAX_PATH], path[MAX_PATH];
    HRESULT co;
    int rc = 0;

    g_auto = pick_env && pick_env[0];
    /* Next to the settings file: %LOCALAPPDATA%\mksm_recomp unless RECOMP_SETTINGS moves it. */
    snprintf(dir, MAX_PATH, "%s", sp && sp[0] ? sp : base_dir);
    if (sp && sp[0] && (slash = strrchr(dir, '\\')) != NULL)
        *slash = '\0';
    to_wide(dir, dest, MAX_PATH);
    _snwprintf_s(game, MAX_PATH, _TRUNCATE, L"%ls\\game", dest);
    _snwprintf_s(partial, MAX_PATH, _TRUNCATE, L"%ls\\game.partial", dest);

    /* An earlier extract whose game_dir= was lost (the settings file reset). */
    if (check_folder(game, game_dir, cap, 1)) {
        fprintf(stderr, "[SETUP] using the earlier extract %s\n", game_dir);
        save_game_dir(game_dir);
        return 1;
    }

    if (!g_auto) {
        wchar_t intro[1024];
        _snwprintf_s(intro, 1024, _TRUNCATE,
                     L"No game files were found.\n\n"
                     L"This port runs your own copy of Mortal Kombat: Shaolin Monks for the "
                     L"original Xbox (the USA/Europe disc). In the next window, pick either\n\n"
                     L"  \x2022  the disc image (.iso): it is extracted once (about 4 GB) into\n"
                     L"      %ls\n"
                     L"  \x2022  or default.xbe in a folder holding the extracted disc.\n\n"
                     L"This is asked only once.", game);
        if (MessageBoxW(NULL, intro, TITLE, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
            log_w(L"setup cancelled");
            return 0;
        }
    }
    co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        if (g_auto) {
            to_wide(pick_env, dest, MAX_PATH);
            if (!GetFullPathNameW(dest, MAX_PATH, path, NULL))
                wcscpy_s(path, MAX_PATH, dest);
        } else {
            int p = pick(path, MAX_PATH);
            if (p < 0)
                say(MB_ICONERROR, L"The file dialog didn't open. Start the game with the game "
                    L"folder as its first argument instead.");
            if (p <= 0) {
                rc = -1;
                break;
            }
        }
        {
            wchar_t line[MAX_PATH + 16];
            _snwprintf_s(line, MAX_PATH + 16, _TRUNCATE, L"picked %ls", path);
            log_w(line);
        }
        rc = try_path(path, game, partial, game_dir, cap);
        if (rc != 0 || g_auto)
            break;
    }
    if (SUCCEEDED(co))
        CoUninitialize();
    if (rc == 1) {
        save_game_dir(game_dir);
        return 1;
    }
    log_w(L"no game folder chosen");
    return 0;
}

int mksm_setup_log_xbe(const char *xbe_path)
{
    wchar_t w[MAX_PATH];
    uint8_t h[32];
    char hex[65];
    int rc, known;

    to_wide(xbe_path, w, MAX_PATH);
    rc = xiso_hash_file(w, L"SHA256", h, sizeof(h));
    if (rc) {
        fprintf(stderr, "[SETUP] default.xbe not hashed (%s)\n", xiso_strerror(rc));
        return 0;
    }
    known = mksm_xbe_is_known(h);
    xiso_hex(h, 32, hex);
    fprintf(stderr, "[SETUP] default.xbe %s (sha256 %s)\n",
            known ? "is the retail USA/Europe build" : "is NOT the build this port is made from",
            hex);
    return known;
}
