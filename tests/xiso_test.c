/* Unit test for src/xiso.c (plan B10b1): the XDVDFS walk on synthetic images (the layout of
 * tests/test_xiso.py), all partition offsets, a file past 4 GB, damaged images, extract to a
 * temp folder with progress and cancel, the free-space check, CNG hashes.
 * CTest `xiso`; tests/test_xiso_c.py runs the built exe too. Prints one line per failure and
 * exits 1 if any.
 * With "--extract <image> <out_dir>" it extracts a real image instead (B10b1 gate). */
#include "xiso.h"

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            failures++;                                        \
        }                                                      \
    } while (0)

#define S 2048u

static uint64_t min64(uint64_t a, uint64_t b) { return a < b ? a : b; }
static uint64_t max64(uint64_t a, uint64_t b) { return a > b ? a : b; }

/* ── Memory image: chunks at offsets, zeros elsewhere ── */
typedef struct Chunk { uint64_t off; uint8_t *data; uint32_t len; } Chunk;
typedef struct Mem { Chunk c[8]; int n; uint64_t size; } Mem;

static int mem_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    Mem *m = ctx;
    memset(buf, 0, len);
    for (int i = 0; i < m->n; i++) {
        uint64_t a = max64(off, m->c[i].off), b = min64(off + len, m->c[i].off + m->c[i].len);
        if (a < b)
            memcpy((uint8_t *)buf + (a - off), m->c[i].data + (a - m->c[i].off), (size_t)(b - a));
    }
    return XISO_OK;
}

static int mem_open(Mem *m, Xiso **x)
{
    XisoSource src = { m, mem_read, m->size };
    return xiso_open_source(&src, x);
}

/* ── test_xiso.py's builders ── */
static size_t dirent(uint8_t *p, const char *name, uint32_t start, uint32_t size, uint8_t attr)
{
    size_t n = strlen(name), len = 14 + n;
    memset(p, 0, 4);
    memcpy(p + 4, &start, 4);                    /* little-endian host */
    memcpy(p + 8, &size, 4);
    p[12] = attr;
    p[13] = (uint8_t)n;
    memcpy(p + 14, name, n);
    while (len % 4)
        p[len++] = 0xFF;
    return len;
}

typedef struct Ent { const char *name; uint32_t start, size; uint8_t attr; } Ent;

/* One sector table, entry i links entry i+1 as its right child, 0xFF after the last.
 * offs (may be NULL) gets each entry's byte offset. */
static void tree(uint8_t *sec, const Ent *e, int n, size_t *offs)
{
    size_t pos = 0, o[16];
    memset(sec, 0xFF, S);
    for (int i = 0; i < n; i++) {
        o[i] = pos;
        pos += dirent(sec + pos, e[i].name, e[i].start, e[i].size, e[i].attr);
    }
    for (int i = 0; i + 1 < n; i++) {
        uint16_t r = (uint16_t)(o[i + 1] / 4);
        sec[o[i] + 2] = (uint8_t)r;
        sec[o[i] + 3] = (uint8_t)(r >> 8);
    }
    if (offs)
        memcpy(offs, o, n * sizeof(*o));
}

static uint8_t g_a[] = "alpha\n";           /* 6 bytes used */
static uint8_t g_b[2560];
static uint8_t g_img[38 * S];
static size_t g_root_offs[3];               /* A.TXT, EMPTY, SUB in the root table */

/* The test_xiso.py image at partition offset base: root (sector 33) = A.TXT (35), EMPTY,
 * SUB (dir at 34) holding B.BIN (b_sector, b_size bytes; data written only when b_size is
 * the real one). The image ends 2 sectors after B.BIN, or at sector 38. */
static void image_ex(Mem *m, uint64_t base, uint32_t b_sector, uint32_t b_size)
{
    Ent sub[] = { { "B.BIN", b_sector, b_size, 0x20 } };
    Ent root[] = { { "A.TXT", 35, 6, 0x20 }, { "EMPTY", 0, 0, 0x10 }, { "SUB", 34, S, 0x10 } };
    for (int i = 0; i < (int)sizeof(g_b); i++)
        g_b[i] = (uint8_t)i;
    memset(g_img, 0, sizeof(g_img));
    memcpy(g_img + 32 * S, "MICROSOFT*XBOX*MEDIA", 20);
    {
        uint32_t r[2] = { 33, S };
        memcpy(g_img + 32 * S + 20, r, 8);
    }
    tree(g_img + 33 * S, root, 3, g_root_offs);
    tree(g_img + 34 * S, sub, 1, NULL);
    memcpy(g_img + 35 * S, g_a, 6);
    m->n = 0;
    m->c[m->n++] = (Chunk){ base, g_img, sizeof(g_img) };
    if (b_sector == 36) {
        memcpy(g_img + 36 * S, g_b, sizeof(g_b));
        m->size = base + sizeof(g_img);
    } else {
        m->c[m->n++] = (Chunk){ base + (uint64_t)b_sector * S, g_b, sizeof(g_b) };
        m->size = base + (uint64_t)b_sector * S + 2 * S;
    }
}

static void image(Mem *m, uint64_t base) { image_ex(m, base, 36, sizeof(g_b)); }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* ── Collect visitor: "path|sector|size|d/f" lines, sorted ── */
typedef struct Collect { char lines[64][96]; int n; } Collect;

static int collect_visit(void *user, const XisoEntry *e)
{
    Collect *c = user;
    if (c->n < 64)
        snprintf(c->lines[c->n++], sizeof(c->lines[0]), "%s|%u|%u|%c", e->path, e->sector,
                 e->size, e->is_dir ? 'd' : 'f');
    return 0;
}

static int cmp_line(const void *a, const void *b) { return strcmp(a, b); }

static const char *const k_expect[] = {
    "A.TXT|35|6|f", "EMPTY|0|0|d", "SUB/B.BIN|36|2560|f", "SUB|34|2048|d",
};

static void check_walk(Xiso *x, const char *what)
{
    static Collect c;
    int rc;
    c.n = 0;
    rc = xiso_walk(x, collect_visit, &c);
    CHECK(rc == XISO_OK, "%s: walk rc %d", what, rc);
    qsort(c.lines, c.n, sizeof(c.lines[0]), cmp_line);
    CHECK(c.n == 4, "%s: %d entries", what, c.n);
    for (int i = 0; i < c.n && i < 4; i++)
        CHECK(strcmp(c.lines[i], k_expect[i]) == 0, "%s: entry %d '%s' != '%s'", what, i,
              c.lines[i], k_expect[i]);
}

static int walk_rc(Mem *m, int *entries)
{
    static Collect c;
    Xiso *x;
    int rc = mem_open(m, &x);
    c.n = 0;
    if (rc == XISO_OK) {
        rc = xiso_walk(x, collect_visit, &c);
        xiso_close(x);
    }
    if (entries)
        *entries = c.n;
    return rc;
}

/* ── Walk tests ── */

static void test_walk_xiso(void)
{
    Mem m;
    Xiso *x;
    image(&m, 0);
    CHECK(mem_open(&m, &x) == XISO_OK, "open xiso");
    if (!x)
        return;
    CHECK(xiso_partition_offset(x) == 0, "partition offset");
    check_walk(x, "xiso");
    xiso_close(x);
}

static void test_walk_redump(void)
{
    static const uint64_t bases[] = { 0x18300000ull, 0x0FD90000ull, 0x02080000ull };
    for (int i = 0; i < 3; i++) {
        Mem m;
        Xiso *x;
        image(&m, bases[i]);
        CHECK(mem_open(&m, &x) == XISO_OK, "open base 0x%llx", (unsigned long long)bases[i]);
        if (!x)
            continue;
        CHECK(xiso_partition_offset(x) == bases[i], "partition offset 0x%llx",
              (unsigned long long)xiso_partition_offset(x));
        check_walk(x, "redump");
        xiso_close(x);
    }
}

static void test_big_offset(void)
{
    Mem m;
    Xiso *x;
    XisoEntry e;
    uint8_t got[sizeof(g_b)];
    image_ex(&m, 0x18300000ull, 0x200010u, sizeof(g_b));   /* B.BIN at 0x118308000 > 4 GB */
    CHECK(mem_open(&m, &x) == XISO_OK, "open big");
    if (!x)
        return;
    CHECK(walk_rc(&m, NULL) == XISO_OK, "walk big");
    CHECK(xiso_find(x, "sub/b.bin", &e) == XISO_OK, "find big");
    CHECK(e.sector == 0x200010u && e.size == sizeof(g_b), "big entry %u %u", e.sector, e.size);
    memset(got, 0, sizeof(got));
    CHECK(xiso_read(x, &e, 0, got, sizeof(got)) == XISO_OK, "read big");
    CHECK(memcmp(got, g_b, sizeof(g_b)) == 0, "big data");
    xiso_close(x);
}

static void test_find(void)
{
    Mem m;
    Xiso *x;
    XisoEntry e;
    uint8_t buf[8];
    image(&m, 0);
    if (mem_open(&m, &x) != XISO_OK) {
        CHECK(0, "open");
        return;
    }
    CHECK(xiso_find(x, "A.TXT", &e) == XISO_OK && !e.is_dir && e.size == 6, "find A.TXT");
    CHECK(xiso_find(x, "a.txt", &e) == XISO_OK && strcmp(e.path, "A.TXT") == 0, "find a.txt");
    CHECK(xiso_read(x, &e, 0, buf, 6) == XISO_OK && memcmp(buf, "alpha\n", 6) == 0, "read A");
    CHECK(xiso_read(x, &e, 2, buf, 5) == XISO_E_TRUNCATED, "read past the file");
    CHECK(xiso_find(x, "SUB/B.BIN", &e) == XISO_OK && e.size == sizeof(g_b), "find SUB/B.BIN");
    CHECK(xiso_find(x, "SUB", &e) == XISO_OK && e.is_dir, "find SUB");
    CHECK(xiso_find(x, "nope", &e) == XISO_E_NOT_FOUND, "find nope");
    {
        uint64_t bytes;
        uint32_t files, dirs;
        CHECK(xiso_totals(x, &bytes, &files, &dirs) == XISO_OK && bytes == 6 + sizeof(g_b) &&
              files == 2 && dirs == 2, "totals");
    }
    xiso_close(x);
}

/* ── Damaged images ── */

static void test_no_descriptor(void)
{
    static uint8_t zeros[40 * S];
    Mem m = { { { 0, zeros, sizeof(zeros) } }, 1, sizeof(zeros) };
    Xiso *x;
    CHECK(mem_open(&m, &x) == XISO_E_NOT_XDVDFS && x == NULL, "no descriptor");
    m.size = 100;                                /* shorter than sector 32 */
    CHECK(mem_open(&m, &x) == XISO_E_NOT_XDVDFS, "tiny image");
}

static void test_truncated(void)
{
    Mem m;
    image_ex(&m, 0, 36, 0x10000000u);            /* B.BIN claims 256 MB */
    CHECK(walk_rc(&m, NULL) == XISO_E_TRUNCATED, "truncated extent");
    image(&m, 0);
    m.size = 36 * S;                             /* B.BIN's data cut off */
    CHECK(walk_rc(&m, NULL) == XISO_E_TRUNCATED, "truncated image");
    image(&m, 0);
    m.size = 33 * S + 100;                       /* the root table cut off */
    CHECK(walk_rc(&m, NULL) == XISO_E_TRUNCATED, "truncated root table");
}

static void test_name_overrun(void)
{
    Mem m;
    image(&m, 0);
    put32(g_img + 32 * S + 24, 20);              /* root table = A.TXT's 20 bytes */
    g_img[33 * S + 13] = 255;                    /* its name runs past the table */
    CHECK(walk_rc(&m, NULL) == XISO_E_CORRUPT, "name overrun");
}

static void test_bad_names(void)
{
    static const char *const bad[] = { "..", ".", "A\\B", "C:", "CON", "con.txt", "COM1", "X.",
                                       "X ", "\x01", "a/b", "A*", "\x80" };
    static const char *const good[] = { "CONSOLE", "COM", "COM0", "a.b", "LPT1X" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        Mem m;
        Ent root[] = { { bad[i], 35, 6, 0x20 } };
        image(&m, 0);
        tree(g_img + 33 * S, root, 1, NULL);
        CHECK(walk_rc(&m, NULL) == XISO_E_BAD_NAME, "bad name #%u", (unsigned)i);
    }
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        Mem m;
        Ent root[] = { { good[i], 35, 6, 0x20 } };
        image(&m, 0);
        tree(g_img + 33 * S, root, 1, NULL);
        CHECK(walk_rc(&m, NULL) == XISO_OK, "good name '%s'", good[i]);
    }
}

static void test_tree_cycle(void)
{
    Mem m;
    int n = 0;
    uint8_t *sub;
    image(&m, 0);
    sub = g_img + 33 * S + g_root_offs[2];
    put16(sub, (uint16_t)(g_root_offs[1] / 4));        /* SUB.left -> EMPTY (seen already) */
    put16(sub + 2, (uint16_t)(g_root_offs[2] / 4));    /* SUB.right -> SUB itself */
    CHECK(walk_rc(&m, &n) == XISO_OK && n == 4, "tree cycle: %d entries", n);
}

static void test_dir_self_loop(void)
{
    Mem m;
    image(&m, 0);
    put32(g_img + 33 * S + g_root_offs[2] + 4, 33);   /* SUB's table = the root's */
    CHECK(walk_rc(&m, NULL) == XISO_E_CORRUPT, "dir self loop");
}

static void test_huge_dir(void)
{
    Mem m;
    Xiso *x;
    image(&m, 0);
    put32(g_img + 32 * S + 24, 32u << 20);
    CHECK(mem_open(&m, &x) == XISO_E_CORRUPT, "huge root dir");
}

/* ── Files ── */

static wchar_t g_tmp[MAX_PATH];             /* this run's temp folder, with a trailing '\' */

static void rm_tree(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            swprintf(p, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                rm_tree(p);
            else
                DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void tmp_path(wchar_t *out, const wchar_t *name)
{
    swprintf(out, MAX_PATH, L"%ls%ls", g_tmp, name);
}

static int write_file(const wchar_t *path, const void *data, uint32_t len)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wrote = 0;
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    ok = WriteFile(h, data, len, &wrote, NULL) && wrote == len;
    CloseHandle(h);
    return ok;
}

static int file_equals(const wchar_t *path, const void *data, uint32_t len)
{
    static uint8_t buf[8192];
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD got = 0;
    int ok;
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    ok = ReadFile(h, buf, sizeof(buf), &got, NULL) && got == len && memcmp(buf, data, len) == 0;
    CloseHandle(h);
    return ok;
}

static int exists(const wchar_t *path, int want_dir)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !!(a & FILE_ATTRIBUTE_DIRECTORY) == want_dir;
}

typedef struct Prog { int calls, cancel, monotonic; uint64_t last_done, last_total; } Prog;

static int prog_cb(void *user, uint64_t done, uint64_t total, const char *path)
{
    Prog *p = user;
    (void)path;
    if (p->calls && done < p->last_done)
        p->monotonic = 0;
    p->calls++;
    p->last_done = done;
    p->last_total = total;
    return p->cancel;
}

/* The xiso image written to a temp file. */
static int open_image_file(Xiso **x)
{
    wchar_t iso[MAX_PATH];
    Mem m;
    image(&m, 0);
    tmp_path(iso, L"test.iso");
    if (!write_file(iso, g_img, sizeof(g_img)))
        return XISO_E_WRITE;
    return xiso_open_file(iso, x);
}

static void test_extract(void)
{
    wchar_t out[MAX_PATH], p[MAX_PATH];
    Prog prog = { 0, 0, 1 };
    unsigned long os_err = 1;
    Xiso *x;
    XisoEntry e;
    uint8_t d1[32], d2[32];
    int rc;

    if (open_image_file(&x) != XISO_OK) {
        CHECK(0, "open image file");
        return;
    }
    CHECK(xiso_partition_offset(x) == 0, "file partition offset");
    tmp_path(out, L"out\\deep\\x");
    rc = xiso_extract(x, out, prog_cb, &prog, &os_err);
    CHECK(rc == XISO_OK && os_err == 0, "extract rc %d (%s) os %lu", rc, xiso_strerror(rc), os_err);
    tmp_path(p, L"out\\deep\\x\\A.TXT");
    CHECK(file_equals(p, "alpha\n", 6), "A.TXT content");
    tmp_path(p, L"out\\deep\\x\\SUB\\B.BIN");
    CHECK(file_equals(p, g_b, sizeof(g_b)), "SUB/B.BIN content");
    tmp_path(p, L"out\\deep\\x\\EMPTY");
    CHECK(exists(p, 1), "EMPTY is a dir");
    CHECK(prog.calls >= 1 && prog.monotonic, "progress calls %d monotonic %d", prog.calls,
          prog.monotonic);
    CHECK(prog.last_done == 6 + sizeof(g_b) && prog.last_total == 6 + sizeof(g_b),
          "progress end %llu/%llu", (unsigned long long)prog.last_done,
          (unsigned long long)prog.last_total);

    /* hash_entry_vs_file */
    tmp_path(p, L"out\\deep\\x\\SUB\\B.BIN");
    CHECK(xiso_find(x, "SUB/B.BIN", &e) == XISO_OK, "find for hash");
    CHECK(xiso_hash_entry(x, &e, L"SHA256", d1, 32) == XISO_OK, "hash entry");
    CHECK(xiso_hash_file(p, L"SHA256", d2, 32) == XISO_OK, "hash file");
    CHECK(memcmp(d1, d2, 32) == 0, "entry hash == file hash");

    /* extracting again over the same folder works (files replaced) */
    CHECK(xiso_extract(x, out, NULL, NULL, NULL) == XISO_OK, "extract again");
    xiso_close(x);
}

static void test_extract_cancel(void)
{
    wchar_t out[MAX_PATH], p[MAX_PATH];
    Prog prog = { 0, 1, 1 };
    Xiso *x;
    int rc;
    if (open_image_file(&x) != XISO_OK) {
        CHECK(0, "open image file");
        return;
    }
    tmp_path(out, L"cancel");
    rc = xiso_extract(x, out, prog_cb, &prog, NULL);
    CHECK(rc == XISO_E_CANCELLED, "cancel rc %d", rc);
    CHECK(prog.calls == 1, "cancel: %d progress calls", prog.calls);
    tmp_path(p, L"cancel\\A.TXT");                 /* the first file, cancelled while written */
    CHECK(!exists(p, 0) && !exists(p, 1), "no partial A.TXT");
    xiso_close(x);
}

static void test_extract_to_file(void)
{
    wchar_t f[MAX_PATH], out[MAX_PATH];
    unsigned long os_err = 0;
    Xiso *x;
    int rc;
    if (open_image_file(&x) != XISO_OK) {
        CHECK(0, "open image file");
        return;
    }
    tmp_path(f, L"afile");
    write_file(f, "x", 1);
    rc = xiso_extract(x, f, NULL, NULL, &os_err);
    CHECK(rc == XISO_E_WRITE && os_err != 0, "extract onto a file: rc %d os %lu", rc, os_err);
    tmp_path(out, L"afile\\sub");
    rc = xiso_extract(x, out, NULL, NULL, NULL);
    CHECK(rc == XISO_E_WRITE, "extract under a file: rc %d", rc);
    xiso_close(x);
}

static void test_space(void)
{
    wchar_t sub[MAX_PATH];
    uint64_t avail = 0, avail2 = 0;
    CHECK(xiso_check_space(g_tmp, 1, &avail) == XISO_OK && avail > 0, "space ok");
    CHECK(xiso_check_space(g_tmp, 1ull << 62, &avail) == XISO_E_NO_SPACE && avail > 0,
          "no space");
    tmp_path(sub, L"not\\there\\yet");
    CHECK(xiso_check_space(sub, 1, &avail2) == XISO_OK && avail2 > 0, "missing folder -> parent");
    CHECK(xiso_space_needed(100, 2, 1) == 100 + 3 * 4096 + (64ull << 20), "space needed");
}

static void test_hash_vectors(void)
{
    wchar_t p[MAX_PATH];
    uint8_t d[32];
    char hex[65];
    tmp_path(p, L"abc.txt");
    write_file(p, "abc", 3);
    CHECK(xiso_hash_file(p, L"SHA256", d, 32) == XISO_OK, "sha256 abc");
    xiso_hex(d, 32, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "sha256 abc = %s", hex);
    CHECK(xiso_hash_file(p, L"SHA1", d, 20) == XISO_OK, "sha1 abc");
    xiso_hex(d, 20, hex);
    CHECK(!strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d"), "sha1 abc = %s", hex);
    CHECK(xiso_hash_file(p, L"SHA256", d, 20) == XISO_E_HASH, "wrong digest length");
    tmp_path(p, L"empty.txt");
    write_file(p, "", 0);
    CHECK(xiso_hash_file(p, L"SHA256", d, 32) == XISO_OK, "sha256 empty");
    xiso_hex(d, 32, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "sha256 empty = %s", hex);
    tmp_path(p, L"missing.txt");
    CHECK(xiso_hash_file(p, L"SHA256", d, 32) == XISO_E_OPEN, "hash missing file");
}

static void test_xbe_known(void)
{
    uint8_t d[32];
    for (int i = 0; i < 32; i++)
        d[i] = (uint8_t)(i * 37 + 11);
    CHECK(!mksm_xbe_is_known(d), "random digest is not known");
    CHECK(strcmp(xiso_strerror(XISO_E_NO_SPACE), "unknown error") != 0, "strerror");
}

/* ── --extract <image> <out_dir>: the real-disc run (B10b1 gate) ── */

static int cli_prog(void *user, uint64_t done, uint64_t total, const char *path)
{
    (void)user;
    printf("\r%6.1f%%  %-60.60s", total ? 100.0 * done / total : 100.0, path);
    fflush(stdout);
    return 0;
}

static int run_extract(const wchar_t *img, const wchar_t *out)
{
    Xiso *x;
    XisoEntry e;
    uint64_t bytes, avail = 0;
    uint32_t files, dirs;
    uint8_t d[32];
    char hex[65];
    unsigned long os_err = 0;
    ULONGLONG t0;
    int rc = xiso_open_file(img, &x);

    if (rc) {
        printf("open: %s\n", xiso_strerror(rc));
        return 1;
    }
    printf("partition offset 0x%llX\n", (unsigned long long)xiso_partition_offset(x));
    rc = xiso_totals(x, &bytes, &files, &dirs);
    printf("totals: %llu bytes, %u files, %u dirs (%s)\n", (unsigned long long)bytes, files, dirs,
           xiso_strerror(rc));
    rc = xiso_find(x, "default.xbe", &e);
    if (rc == XISO_OK)
        rc = xiso_hash_entry(x, &e, L"SHA256", d, 32);
    if (rc == XISO_OK) {
        xiso_hex(d, 32, hex);
        printf("default.xbe sha256 %s (%s)\n", hex, mksm_xbe_is_known(d) ? "known" : "UNKNOWN");
    } else
        printf("default.xbe: %s\n", xiso_strerror(rc));
    xiso_check_space(out, xiso_space_needed(bytes, files, dirs), &avail);
    printf("free %llu, need %llu\n", (unsigned long long)avail,
           (unsigned long long)xiso_space_needed(bytes, files, dirs));
    t0 = GetTickCount64();
    rc = xiso_extract(x, out, cli_prog, NULL, &os_err);
    printf("\nextract: %s (os %lu) in %.1f s\n", xiso_strerror(rc), os_err,
           (GetTickCount64() - t0) / 1000.0);
    xiso_close(x);
    return rc ? 1 : 0;
}

int main(void)
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t base[MAX_PATH];

    if (argv && argc == 4 && !wcscmp(argv[1], L"--extract"))
        return run_extract(argv[2], argv[3]);

    GetTempPathW(MAX_PATH, base);
    swprintf(g_tmp, MAX_PATH, L"%lsxiso_test_%lu\\", base, GetCurrentProcessId());
    CreateDirectoryW(g_tmp, NULL);

    test_walk_xiso();
    test_walk_redump();
    test_big_offset();
    test_find();
    test_no_descriptor();
    test_truncated();
    test_name_overrun();
    test_bad_names();
    test_tree_cycle();
    test_dir_self_loop();
    test_huge_dir();
    test_extract();
    test_extract_cancel();
    test_extract_to_file();
    test_space();
    test_hash_vectors();
    test_xbe_known();

    g_tmp[wcslen(g_tmp) - 1] = L'\0';
    rm_tree(g_tmp);
    if (failures) {
        printf("xiso_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("xiso_test: OK\n");
    return 0;
}
