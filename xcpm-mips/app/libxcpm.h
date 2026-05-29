/*============================================================================
 * libxcpm.h -- tiny user-facing header for XCP/M-MIPS apps.
 *
 * Apps include this and define:
 *
 *     int app_main(bdos_fn_t bdos) { ... }
 *
 * The kernel-side loader (glue_ncd15.c run_app_from_fat or
 * pgmld_run_mips) calls _app_start at TPA base (= linker ENTRY), which
 * trampolines to app_main(bdos) with $a0 = bdos_dispatch. No syscalls,
 * no kernel includes -- just a function pointer.
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

#endif /* LIBXCPM_H */
