/* POSIX (plan H.5c2a): the Ghidra naming pass gives some lifted CRT functions plain libc names.
 * MSVC's CRT spells its own _write/_close, so on Windows they're harmless; on Linux the exe's
 * definition wins over libc's for every caller in the exe (the toolkit's I/O, stdio) and, with
 * the exports, for shared libraries too, which then run guest code. Force-included into
 * src/recomp/gen/*.c only (CMakeLists.txt), so the lifted functions get other names and the
 * toolkit keeps libc's. A new lift can add names: list the exe's exports that libc also
 * defines (linux-build.md, "libc names") and add them here. */
#ifndef MKSM_RECOMP_LIBC_NAMES_H
#define MKSM_RECOMP_LIBC_NAMES_H

#define close  guest_close
#define fsopen guest_fsopen
#define isatty guest_isatty
#define lseek  guest_lseek
#define read   guest_read
#define tzset  guest_tzset
#define write  guest_write

#endif
