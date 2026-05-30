/*============================================================================
 * libxcpm.h -- tiny user-facing header for XCP/M-MIPS apps.
 *
 * Apps include this and define:
 *
 *     int app_main(bdos_fn_t bdos, const char *tail) { ... }
 *
 * The kernel-side loader calls _app_start at TPA base (= linker ENTRY),
 * which trampolines to app_main(bdos, tail) with $a0 = bdos_dispatch
 * and $a1 = the CCP command tail (the bit after the command name --
 * args + redirects). For apps that don't care about args, `tail` may
 * be `""`; the parameter is positional so old single-arg signatures
 * still work, the extra register just gets ignored.
 *==========================================================================*/
#ifndef LIBXCPM_H
#define LIBXCPM_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* BDOS function pointer the loader passes in $a0. */
typedef u32 (*bdos_fn_t)(u32 func, u32 p1, u32 p2);

/* BDOS function numbers used by the convenience wrappers below. See
 * src/bdos.c for the full table. */
#define BDOS_CONIN       1   /* console input, returns char in v0 */
#define BDOS_CONOUT      2   /* console output, p1 = char */
#define BDOS_DIRCONIO    6   /* direct console I/O (0xFF = in, else out) */
#define BDOS_PRINTSTR    9   /* print dollar-terminated string, p1 = ptr */
#define BDOS_VERSION    12   /* returns 0x0022 on this kernel */
#define BDOS_IDENT     100   /* returns 0x58504D4B ("XPMK") */

/* XCP/M extension calls (see src/bdos.c d_ext_* table). */
#define BDOS_LISTROOT  110   /* print A: dir to console (p1 = 0) */
#define BDOS_CAT       111   /* print file p1 (NUL-term path) to console */
#define BDOS_GETCWD    125   /* copy cwd into caller's buffer at p1 */

/*-- Convenience wrappers ------------------------------------------------
 * All take the bdos function pointer as the first arg so apps can call
 * them without a global. Inline so a minimal app builds with no
 * libxcpm.a -- just the header. */

static inline void xputc(bdos_fn_t bdos, char c)
{
    bdos(BDOS_CONOUT, (u32)(u8)c, 0);
}

/* Print a NUL-terminated string char-by-char (BDOS 2). Use this when
 * your string may contain a literal dollar-sign; otherwise use xputs_d
 * for the cheaper dollar-terminated BDOS 9 path. */
static inline void xputs(bdos_fn_t bdos, const char *s)
{
    while (*s) bdos(BDOS_CONOUT, (u32)(u8)*s++, 0);
}

/* Print a dollar-terminated string via BDOS 9. Faster (one call) but
 * the string body must not contain a dollar-sign byte. */
static inline void xputs_d(bdos_fn_t bdos, const char *s)
{
    bdos(BDOS_PRINTSTR, (u32)s, 0);
}

/* Print a u32 as 8 uppercase hex digits, no prefix, no newline. */
static inline void xputhex8(bdos_fn_t bdos, u32 v)
{
    static const char digits[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
        bdos(BDOS_CONOUT, (u32)(u8)digits[(v >> i) & 0xF], 0);
}

/* Print a u32 as decimal, no prefix, no newline. */
static inline void xputdec(bdos_fn_t bdos, u32 v)
{
    char buf[12];
    int i = 0;
    if (v == 0) { bdos(BDOS_CONOUT, '0', 0); return; }
    while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) bdos(BDOS_CONOUT, (u32)(u8)buf[i], 0);
}

static inline char xgetc(bdos_fn_t bdos)
{
    return (char)bdos(BDOS_CONIN, 0, 0);
}

/*-- syscall ABI ---------------------------------------------------------
 * Position-independent alternative to the function-pointer ABI. Apps
 * that prefer not to thread `bdos` through their callgraph can issue a
 * MIPS `syscall` instruction with $v0 = func, $a0 = p1, $a1 = p2; the
 * kernel handler at 0x80000080 (installed at boot by start.S) marshals
 * those into bdos_dispatch and returns the result in $v0.
 *
 * Apps using bdos_sys() don't need the loader's $a0 = bdos pointer, so
 * their `app_main` signature can be `int app_main(void)` and the
 * binary is fully position-independent within KSEG0. */
static inline u32 bdos_sys(u32 func, u32 p1, u32 p2)
{
    register u32 v0 asm("v0") = func;
    register u32 a0 asm("a0") = p1;
    register u32 a1 asm("a1") = p2;
    asm volatile("syscall"
                 : "+r"(v0)
                 : "r"(a0), "r"(a1)
                 : "memory");
    return v0;
}

#endif /* LIBXCPM_H */
