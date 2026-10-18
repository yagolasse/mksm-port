/* XDVDFS reader and extractor (plan B10b1); see xiso.h. Port of tools/xiso.py, plus: 64-bit
 * offsets, bounds checks on every table and extent, a nesting limit, name checks (no
 * writing outside the output folder), a free-space check, progress, CNG hashes. */
#include "xiso.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef BCRYPT_SUCCESS
#define BCRYPT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

#define SECTOR        2048u
#define VD_SECTOR     32u
#define MAX_DEPTH     32
#define MAX_DIR_BYTES (16u << 20)     /* a directory table larger than this is corrupt */
#define PATH_MAX_A    1024
#define COPY_CHUNK    (1u << 20)

static const char k_magic[20] = { 'M','I','C','R','O','S','O','F','T','*',
                                  'X','B','O','X','*','M','E','D','I','A' };
/* Trimmed xiso, then the full-disc game partition offsets (tools/xiso.py PARTITION_OFFSETS). */
static const uint64_t k_bases[] = { 0, 0x18300000ull, 0x0FD90000ull, 0x02080000ull };

/* SHA-256 of the retail default.xbe builds the recompile is tied to: the USA/Europe Xbox disc
 * (one XBE for both regions). One line per further known build. */
static const char *const k_xbe_sha256[] = {
    "48240902bf4a04eb89376217c529531032d3370749bcce91a194878cc49db74a",
};

struct Xiso {
    XisoSource src;
    HANDLE file;              /* xiso_open_file only */
    uint64_t base;
    uint32_t root_sector, root_size;
    char find_path[PATH_MAX_A];
};

static uint64_t min64(uint64_t a, uint64_t b) { return a < b ? a : b; }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Every image read goes through here: bounds first, then the source. */
static int read_at(Xiso *x, uint64_t off, void *buf, uint32_t len)
{
    if (off > x->src.size || len > x->src.size - off)
        return XISO_E_TRUNCATED;
    return len ? x->src.read(x->src.ctx, off, buf, len) : XISO_OK;
}

static uint64_t sector_off(const Xiso *x, uint32_t sector)
{
    return x->base + (uint64_t)sector * SECTOR;   /* 64-bit: images are > 4 GB */
}

/* ── Sources ───────────────────────────────────────────── */

static int file_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    OVERLAPPED ov = { 0 };
    DWORD got = 0;
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    if (!ReadFile((HANDLE)ctx, buf, len, &got, &ov))
        return GetLastError() == ERROR_HANDLE_EOF ? XISO_E_TRUNCATED : XISO_E_READ;
    return got == len ? XISO_OK : XISO_E_TRUNCATED;
}

int xiso_open_source(const XisoSource *src, Xiso **out)
{
    Xiso *x = calloc(1, sizeof(*x));
    uint8_t vd[28];

    *out = NULL;
    if (!x)
        return XISO_E_NOMEM;
    x->src = *src;
    x->file = INVALID_HANDLE_VALUE;
    for (size_t i = 0; i < sizeof(k_bases) / sizeof(k_bases[0]); i++) {
        x->base = k_bases[i];
        if (read_at(x, sector_off(x, VD_SECTOR), vd, sizeof(vd)) != XISO_OK)
            continue;
        if (memcmp(vd, k_magic, sizeof(k_magic)) != 0)
            continue;
        x->root_sector = rd32(vd + 20);
        x->root_size = rd32(vd + 24);
        if (x->root_size > MAX_DIR_BYTES) {
            free(x);
            return XISO_E_CORRUPT;
        }
        *out = x;
        return XISO_OK;
    }
    free(x);
    return XISO_E_NOT_XDVDFS;
}

int xiso_open_file(const wchar_t *path, Xiso **out)
{
    LARGE_INTEGER size;
    XisoSource src;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    int rc;

    *out = NULL;
    if (h == INVALID_HANDLE_VALUE)
        return XISO_E_OPEN;
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return XISO_E_READ;
    }
    src.ctx = h;
    src.read = file_read;
    src.size = (uint64_t)size.QuadPart;
    rc = xiso_open_source(&src, out);
    if (rc != XISO_OK)
        CloseHandle(h);
    else
        (*out)->file = h;
    return rc;
}

void xiso_close(Xiso *x)
{
    if (!x)
        return;
    if (x->file != INVALID_HANDLE_VALUE)
        CloseHandle(x->file);
    free(x);
}

uint64_t xiso_partition_offset(const Xiso *x) { return x->base; }

/* ── Walk ──────────────────────────────────────────────── */

/* A plain file/dir name: printable ASCII, none of \ / : * ? " < > |, not "." or "..",
 * no trailing dot or space (Windows drops them), no device names (CON, CON.TXT, COM1...). */
static int name_ok(const uint8_t *n, unsigned len)
{
    static const char *const dev3[] = { "CON", "PRN", "AUX", "NUL" };
    if (len == 0)
        return 0;
    for (unsigned i = 0; i < len; i++) {
        if (n[i] < 0x20 || n[i] > 0x7E || strchr("\\/:*?\"<>|", n[i]))
            return 0;
    }
    if (n[len - 1] == '.' || n[len - 1] == ' ')
        return 0;                                   /* also catches "." and ".." */
    for (int i = 0; i < 4; i++)
        if ((len == 3 || (len > 3 && n[3] == '.')) && _strnicmp((const char *)n, dev3[i], 3) == 0)
            return 0;
    if (len >= 4 && (len == 4 || n[4] == '.') &&
        (_strnicmp((const char *)n, "COM", 3) == 0 || _strnicmp((const char *)n, "LPT", 3) == 0) &&
        n[3] >= '1' && n[3] <= '9')
        return 0;
    return 1;
}

typedef struct Walk {
    Xiso *x;
    XisoVisitFn fn;
    void *user;
    char path[PATH_MAX_A];
} Walk;

static int walk_dir(Walk *w, uint32_t sector, uint32_t size, size_t plen, int depth)
{
    uint8_t *data, *seen;
    uint32_t *stack;
    uint32_t sp = 0, cap;
    int rc;

    if (size == 0)
        return XISO_OK;                              /* empty directory */
    if (depth > MAX_DEPTH || size > MAX_DIR_BYTES)
        return XISO_E_CORRUPT;
    cap = (size / 4) * 2 + 2;                        /* each entry pushes at most 2 */
    data = malloc(size);
    seen = calloc(size / 4 + 1, 1);
    stack = malloc(cap * sizeof(*stack));
    if (!data || !seen || !stack) {
        rc = XISO_E_NOMEM;
        goto out;
    }
    rc = read_at(w->x, sector_off(w->x, sector), data, size);
    if (rc != XISO_OK)
        goto out;

    stack[sp++] = 0;
    while (sp) {
        uint32_t off = stack[--sp];
        uint16_t left, right;
        uint8_t attr, nlen;
        XisoEntry e;
        size_t n;

        if ((uint64_t)off + 14 > size || seen[off / 4])
            continue;
        seen[off / 4] = 1;
        left = rd16(data + off);
        right = rd16(data + off + 2);
        if (left == 0xFFFF)                          /* 0xFF padding to the sector end */
            continue;
        attr = data[off + 12];
        nlen = data[off + 13];
        if (nlen == 0 || (uint64_t)off + 14 + nlen > size) {
            rc = XISO_E_CORRUPT;
            break;
        }
        if (!name_ok(data + off + 14, nlen)) {
            rc = XISO_E_BAD_NAME;
            break;
        }
        n = plen;
        if (plen)
            w->path[n++] = '/';
        if (n + nlen >= sizeof(w->path)) {
            rc = XISO_E_CORRUPT;
            break;
        }
        memcpy(w->path + n, data + off + 14, nlen);
        w->path[n + nlen] = '\0';

        e.path = w->path;
        e.sector = rd32(data + off + 4);
        e.size = rd32(data + off + 8);
        e.is_dir = (attr & 0x10) != 0;
        if (!e.is_dir && e.size) {
            uint64_t start = sector_off(w->x, e.sector);
            if (start > w->x->src.size || e.size > w->x->src.size - start) {
                rc = XISO_E_TRUNCATED;
                break;
            }
        }
        rc = w->fn ? w->fn(w->user, &e) : 0;
        if (rc)
            break;
        if (e.is_dir) {
            rc = walk_dir(w, e.sector, e.size, n + nlen, depth + 1);
            if (rc)
                break;
        }
        w->path[plen] = '\0';
        /* tools/xiso.py's order: left pushed first, so right is visited first. */
        if (left && sp < cap)
            stack[sp++] = (uint32_t)left * 4u;
        if (right && sp < cap)
            stack[sp++] = (uint32_t)right * 4u;
    }
out:
    free(stack);
    free(seen);
    free(data);
    return rc;
}

int xiso_walk(Xiso *x, XisoVisitFn fn, void *user)
{
    Walk *w = malloc(sizeof(*w));    /* 1 KB path: off the caller's stack */
    int rc;
    if (!w)
        return XISO_E_NOMEM;
    w->x = x;
    w->fn = fn;
    w->user = user;
    w->path[0] = '\0';
    rc = walk_dir(w, x->root_sector, x->root_size, 0, 0);
    free(w);
    return rc;
}

/* ── Find / read / totals ──────────────────────────────── */

/* Internal visitors return XISO_E_* codes or FOUND, so the two can't be confused. */
#define FOUND (-1)

typedef struct Find { const char *want; XisoEntry hit; char *path; size_t cap; } Find;

static int find_visit(void *user, const XisoEntry *e)
{
    Find *f = user;
    if (_stricmp(e->path, f->want) != 0)
        return 0;
    f->hit = *e;
    snprintf(f->path, f->cap, "%s", e->path);
    f->hit.path = f->path;
    return FOUND;
}

int xiso_find(Xiso *x, const char *path, XisoEntry *out)
{
    Find f = { path, { 0 }, x->find_path, sizeof(x->find_path) };
    int rc = xiso_walk(x, find_visit, &f);
    if (rc == FOUND) {
        *out = f.hit;
        return XISO_OK;
    }
    return rc ? rc : XISO_E_NOT_FOUND;
}

int xiso_read(Xiso *x, const XisoEntry *e, uint64_t offset, void *buf, uint32_t len)
{
    if (e->is_dir || offset > e->size || len > e->size - offset)
        return XISO_E_TRUNCATED;
    return read_at(x, sector_off(x, e->sector) + offset, buf, len);
}

typedef struct Totals { uint64_t bytes; uint32_t files, dirs; } Totals;

static int totals_visit(void *user, const XisoEntry *e)
{
    Totals *t = user;
    if (e->is_dir)
        t->dirs++;
    else {
        t->files++;
        t->bytes += e->size;
    }
    return 0;
}

int xiso_totals(Xiso *x, uint64_t *bytes, uint32_t *files, uint32_t *dirs)
{
    Totals t = { 0 };
    int rc = xiso_walk(x, totals_visit, &t);
    if (bytes) *bytes = t.bytes;
    if (files) *files = t.files;
    if (dirs) *dirs = t.dirs;
    return rc;
}

/* ── Space ─────────────────────────────────────────────── */

uint64_t xiso_space_needed(uint64_t bytes, uint32_t files, uint32_t dirs)
{
    /* Upper bound for 4 KiB clusters: every file wastes < 4 KiB. */
    return bytes + (uint64_t)files * 4096u + (uint64_t)dirs * 4096u + (64ull << 20);
}

int xiso_check_space(const wchar_t *dir, uint64_t need, uint64_t *avail)
{
    wchar_t probe[MAX_PATH];
    ULARGE_INTEGER free_caller;

    if (avail)
        *avail = 0;
    if (wcslen(dir) >= MAX_PATH)
        return XISO_E_PATH;
    wcscpy_s(probe, MAX_PATH, dir);
    /* The folder may not exist yet: ask about its nearest existing parent. */
    while (GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES) {
        wchar_t *slash = wcsrchr(probe, L'\\');
        if (!slash || slash == probe)
            return XISO_E_WRITE;
        *slash = L'\0';
        if (wcslen(probe) == 2 && probe[1] == L':')  /* "C:" → "C:\" */
            wcscat_s(probe, MAX_PATH, L"\\");
    }
    if (!GetDiskFreeSpaceExW(probe, &free_caller, NULL, NULL))
        return XISO_E_WRITE;
    if (avail)
        *avail = free_caller.QuadPart;
    return free_caller.QuadPart >= need ? XISO_OK : XISO_E_NO_SPACE;
}

/* ── Extract ───────────────────────────────────────────── */

typedef struct Extract {
    Xiso *x;
    const wchar_t *out_dir;
    XisoProgressFn fn;
    void *user;
    uint64_t done, total;
    ULONGLONG last_tick;
    unsigned long os_error;
    uint8_t *buf;
} Extract;

/* out_dir + '\' + disc path with '/' → '\' (names are checked ASCII, so a plain widening). */
static int host_path(const Extract *ex, const char *path, wchar_t *out, size_t cap)
{
    size_t n = wcslen(ex->out_dir), m = strlen(path);
    if (n + 1 + m + 1 > cap)
        return XISO_E_PATH;
    wmemcpy(out, ex->out_dir, n);
    out[n++] = L'\\';
    for (size_t i = 0; i < m; i++)
        out[n + i] = path[i] == '/' ? L'\\' : (wchar_t)(unsigned char)path[i];
    out[n + m] = L'\0';
    return XISO_OK;
}

static int progress(Extract *ex, const char *path, int force)
{
    ULONGLONG now = GetTickCount64();
    if (!ex->fn || (!force && now - ex->last_tick < 50))
        return 0;
    ex->last_tick = now;
    return ex->fn(ex->user, ex->done, ex->total, path);
}

static int extract_visit(void *user, const XisoEntry *e)
{
    Extract *ex = user;
    wchar_t dst[MAX_PATH];
    HANDLE h;
    uint64_t off = 0;
    int rc = host_path(ex, e->path, dst, MAX_PATH);

    if (rc)
        return rc;
    if (e->is_dir) {
        /* Pre-order walk: the parent exists already. */
        if (!CreateDirectoryW(dst, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            ex->os_error = GetLastError();
            return XISO_E_WRITE;
        }
        return 0;
    }
    h = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        ex->os_error = GetLastError();
        return XISO_E_WRITE;
    }
    while (off < e->size) {
        uint32_t len = (uint32_t)min64(COPY_CHUNK, e->size - off);
        DWORD wrote = 0;
        rc = xiso_read(ex->x, e, off, ex->buf, len);
        if (rc)
            break;
        if (!WriteFile(h, ex->buf, len, &wrote, NULL) || wrote != len) {
            ex->os_error = GetLastError();
            rc = XISO_E_WRITE;
            break;
        }
        off += len;
        ex->done += len;
        if (progress(ex, e->path, 0)) {
            rc = XISO_E_CANCELLED;
            break;
        }
    }
    CloseHandle(h);
    if (rc)
        DeleteFileW(dst);                            /* no half-written file left behind */
    return rc;
}

int xiso_extract(Xiso *x, const wchar_t *out_dir, XisoProgressFn fn, void *user,
                 unsigned long *os_error)
{
    Extract ex = { x, out_dir, fn, user };
    uint64_t bytes, avail;
    uint32_t files, dirs;
    DWORD attr;
    int rc, err;

    if (os_error)
        *os_error = 0;
    rc = xiso_totals(x, &bytes, &files, &dirs);     /* also validates the whole tree first */
    if (rc)
        return rc;
    attr = GetFileAttributesW(out_dir);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (os_error)
            *os_error = ERROR_DIRECTORY;
        return XISO_E_WRITE;                         /* out_dir is a file */
    }
    rc = xiso_check_space(out_dir, xiso_space_needed(bytes, files, dirs), &avail);
    if (rc)
        return rc;
    err = SHCreateDirectoryExW(NULL, out_dir, NULL); /* absolute path, any depth */
    if (err != ERROR_SUCCESS && err != ERROR_ALREADY_EXISTS && err != ERROR_FILE_EXISTS) {
        if (os_error)
            *os_error = (unsigned long)err;
        return XISO_E_WRITE;
    }
    attr = GetFileAttributesW(out_dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (os_error)
            *os_error = ERROR_DIRECTORY;
        return XISO_E_WRITE;
    }
    ex.total = bytes;
    ex.buf = malloc(COPY_CHUNK);
    if (!ex.buf)
        return XISO_E_NOMEM;
    rc = xiso_walk(x, extract_visit, &ex);
    if (rc == XISO_OK && progress(&ex, "", 1))
        rc = XISO_E_CANCELLED;                       /* final call: done == total */
    free(ex.buf);
    if (os_error)
        *os_error = ex.os_error;
    return rc;
}

/* ── Hashes (CNG) ──────────────────────────────────────── */

typedef struct Hasher { BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; } Hasher;

static int hasher_open(Hasher *hs, const wchar_t *alg, uint32_t out_len)
{
    DWORD len = 0, got = 0;
    hs->alg = NULL;
    hs->h = NULL;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hs->alg, alg, NULL, 0))) {
        hs->alg = NULL;
        return XISO_E_HASH;
    }
    /* Hash object memory managed by CNG (NULL/0: Windows 7+). */
    if (!BCRYPT_SUCCESS(BCryptGetProperty(hs->alg, BCRYPT_HASH_LENGTH, (PUCHAR)&len,
                                          sizeof(len), &got, 0)) || len != out_len ||
        !BCRYPT_SUCCESS(BCryptCreateHash(hs->alg, &hs->h, NULL, 0, NULL, 0, 0))) {
        BCryptCloseAlgorithmProvider(hs->alg, 0);
        hs->alg = NULL;
        return XISO_E_HASH;
    }
    return XISO_OK;
}

static int hasher_finish(Hasher *hs, uint8_t *out, uint32_t out_len, int rc)
{
    if (rc == XISO_OK && !BCRYPT_SUCCESS(BCryptFinishHash(hs->h, out, out_len, 0)))
        rc = XISO_E_HASH;
    BCryptDestroyHash(hs->h);
    BCryptCloseAlgorithmProvider(hs->alg, 0);
    return rc;
}

int xiso_hash_entry(Xiso *x, const XisoEntry *e, const wchar_t *alg, uint8_t *out,
                    uint32_t out_len)
{
    Hasher hs;
    uint8_t *buf;
    uint64_t off = 0;
    int rc = hasher_open(&hs, alg, out_len);
    if (rc)
        return rc;
    buf = malloc(COPY_CHUNK);
    if (!buf)
        return hasher_finish(&hs, out, out_len, XISO_E_NOMEM);
    while (rc == XISO_OK && off < e->size) {
        uint32_t len = (uint32_t)min64(COPY_CHUNK, e->size - off);
        rc = xiso_read(x, e, off, buf, len);
        if (rc == XISO_OK && !BCRYPT_SUCCESS(BCryptHashData(hs.h, buf, len, 0)))
            rc = XISO_E_HASH;
        off += len;
    }
    free(buf);
    return hasher_finish(&hs, out, out_len, rc);
}

int xiso_hash_file(const wchar_t *path, const wchar_t *alg, uint8_t *out, uint32_t out_len)
{
    Hasher hs;
    uint8_t *buf;
    DWORD got;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    int rc;
    if (h == INVALID_HANDLE_VALUE)
        return XISO_E_OPEN;
    rc = hasher_open(&hs, alg, out_len);
    buf = rc ? NULL : malloc(COPY_CHUNK);
    if (!rc && !buf)
        rc = XISO_E_NOMEM;
    while (rc == XISO_OK) {
        if (!ReadFile(h, buf, COPY_CHUNK, &got, NULL)) {
            rc = XISO_E_READ;
            break;
        }
        if (got == 0)
            break;
        if (!BCRYPT_SUCCESS(BCryptHashData(hs.h, buf, got, 0)))
            rc = XISO_E_HASH;
    }
    free(buf);
    CloseHandle(h);
    if (hs.alg)                                      /* opened */
        rc = hasher_finish(&hs, out, out_len, rc);
    return rc;
}

void xiso_hex(const uint8_t *d, uint32_t len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < len; i++) {
        out[2 * i] = hex[d[i] >> 4];
        out[2 * i + 1] = hex[d[i] & 15];
    }
    out[2 * len] = '\0';
}

int mksm_xbe_is_known(const uint8_t sha256[32])
{
    char hex[65];
    xiso_hex(sha256, 32, hex);
    for (size_t i = 0; i < sizeof(k_xbe_sha256) / sizeof(k_xbe_sha256[0]); i++)
        if (_stricmp(hex, k_xbe_sha256[i]) == 0)
            return 1;
    return 0;
}

const char *xiso_strerror(int code)
{
    switch (code) {
    case XISO_OK:           return "ok";
    case XISO_E_OPEN:       return "can't open the file";
    case XISO_E_NOT_XDVDFS: return "not an Xbox disc image";
    case XISO_E_CORRUPT:    return "the disc image is damaged (directory)";
    case XISO_E_TRUNCATED:  return "the disc image is incomplete";
    case XISO_E_BAD_NAME:   return "the disc image has an invalid file name";
    case XISO_E_READ:       return "read error";
    case XISO_E_WRITE:      return "can't write the game files";
    case XISO_E_NO_SPACE:   return "not enough free disk space";
    case XISO_E_PATH:       return "path too long";
    case XISO_E_NOT_FOUND:  return "file not found on the disc";
    case XISO_E_CANCELLED:  return "cancelled";
    case XISO_E_NOMEM:      return "out of memory";
    case XISO_E_HASH:       return "hash failed";
    default:                return "unknown error";
    }
}
