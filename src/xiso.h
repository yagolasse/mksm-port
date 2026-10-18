/* XDVDFS (Xbox disc) reader and extractor (plan B10b1), ported from tools/xiso.py.
 * Reads a trimmed xiso (volume at offset 0) or a full disc image (game partition after the
 * video partition), walks the directory trees, extracts into a folder with a progress
 * callback and a free-space check, and hashes a file (Windows CNG) for the default.xbe
 * version check. No UI. Unit test: tests/xiso_test.c (CTest `xiso`). */
#ifndef MKSM_XISO_H
#define MKSM_XISO_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XISO_OK = 0,
    XISO_E_OPEN,          /* the image file can't be opened */
    XISO_E_NOT_XDVDFS,    /* no volume descriptor at any known partition offset */
    XISO_E_CORRUPT,       /* a directory table or entry out of bounds, nesting too deep */
    XISO_E_TRUNCATED,     /* data past the end of the image */
    XISO_E_BAD_NAME,      /* a name that isn't a plain file name (.., \, :, control chars) */
    XISO_E_READ,          /* image read error */
    XISO_E_WRITE,         /* output create/write error (GetLastError in *os_error) */
    XISO_E_NO_SPACE,      /* not enough free space for the extract */
    XISO_E_PATH,          /* output path too long */
    XISO_E_NOT_FOUND,     /* xiso_find: no such entry */
    XISO_E_CANCELLED,     /* the progress callback asked to stop */
    XISO_E_NOMEM,
    XISO_E_HASH,          /* CNG failure */
};

/* Where the image bytes come from: a file (xiso_open_file) or anything else (the unit test's
 * in-memory images, which place a partition past 4 GB without writing 4 GB). read returns
 * XISO_OK or an XISO_E_ code; it is only called for ranges inside [0, size). */
typedef struct XisoSource {
    void *ctx;
    int (*read)(void *ctx, uint64_t offset, void *buf, uint32_t len);
    uint64_t size;
} XisoSource;

typedef struct XisoEntry {
    const char *path;     /* "SUB/B.BIN"; valid during the callback only */
    uint32_t sector;      /* relative to the partition */
    uint32_t size;
    int is_dir;
} XisoEntry;

typedef struct Xiso Xiso;

int  xiso_open_file(const wchar_t *path, Xiso **out);
int  xiso_open_source(const XisoSource *src, Xiso **out);   /* src is copied */
void xiso_close(Xiso *x);
uint64_t xiso_partition_offset(const Xiso *x);
const char *xiso_strerror(int code);

/* Calls fn for every entry, a directory before its contents. fn returns nonzero to stop
 * (that value is returned). Fails on the first bad table/name/extent. */
typedef int (*XisoVisitFn)(void *user, const XisoEntry *e);
int xiso_walk(Xiso *x, XisoVisitFn fn, void *user);

/* Case-insensitive lookup by '/'-path ("default.xbe"). out->path points into x until the
 * next call. */
int xiso_find(Xiso *x, const char *path, XisoEntry *out);
int xiso_read(Xiso *x, const XisoEntry *e, uint64_t offset, void *buf, uint32_t len);

/* Totals over all files (for the progress bar and the space check). */
int xiso_totals(Xiso *x, uint64_t *bytes, uint32_t *files, uint32_t *dirs);

/* Bytes needed on disk for the extract: each file rounded up to 4 KiB clusters, 4 KiB per
 * directory, plus a 64 MiB margin. */
uint64_t xiso_space_needed(uint64_t bytes, uint32_t files, uint32_t dirs);
/* XISO_OK when the volume holding dir (or its nearest existing parent) has need bytes free
 * for this user (GetDiskFreeSpaceExW); *avail gets the free bytes either way (0 on error). */
int xiso_check_space(const wchar_t *dir, uint64_t need, uint64_t *avail);

/* done/total in bytes, path = the file being written. Nonzero = cancel. Called at most
 * every ~50 ms, and once at the end with done == total. */
typedef int (*XisoProgressFn)(void *user, uint64_t done, uint64_t total, const char *path);
/* Extracts everything under out_dir (created, any depth). Checks the space first. On an error
 * or cancel the file being written is deleted, the rest stays: the caller extracts into a
 * temp folder and renames it on success (B10b2). os_error (may be NULL) gets GetLastError
 * for XISO_E_WRITE. */
int xiso_extract(Xiso *x, const wchar_t *out_dir, XisoProgressFn fn, void *user,
                 unsigned long *os_error);

/* Hashes (CNG): alg = L"SHA1" or L"SHA256" (BCRYPT_SHA1_ALGORITHM / BCRYPT_SHA256_ALGORITHM),
 * out_len 20 / 32. From the image (no extract needed) or from a file. */
int xiso_hash_entry(Xiso *x, const XisoEntry *e, const wchar_t *alg, uint8_t *out, uint32_t out_len);
int xiso_hash_file(const wchar_t *path, const wchar_t *alg, uint8_t *out, uint32_t out_len);
void xiso_hex(const uint8_t *digest, uint32_t len, char *out /* 2*len+1 */);

/* The retail default.xbe this recompile is tied to (USA/Europe Xbox disc). */
int mksm_xbe_is_known(const uint8_t sha256[32]);

#ifdef __cplusplus
}
#endif
#endif
