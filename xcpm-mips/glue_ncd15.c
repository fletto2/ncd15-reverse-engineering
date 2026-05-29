/*============================================================================
 * glue_ncd15.c -- MIPS/NCD15 replacements for the 68K-specific glue that
 * normally lives in main.c + pgmld_jump.s.
 *
 * Milestone 2: warmboot is a stub (no CCP wired yet); the program loader
 * returns failure. Both get real bodies in the CCP / app-loader milestone.
 *==========================================================================*/
#include "xcpm.h"
#include "fat.h"

/* Referenced by bdos.c d_reset / d_ext (warm-boot BDOS calls) via
 * `j warmboot`. Until the CCP is wired, just announce and idle in an
 * echo loop so the console stays alive. */
void warmboot(void)
{
    uart_puts("\r\n[warmboot — no CCP yet; console idle]\r\n> ");
    for (;;) {
        int c = uart_getc();
        if (c == '\r') uart_puts("\r\n> ");
        else           uart_putc(c);
    }
}

/* CCP embedded help/stty blobs. On 68K these are .incbin'd compressed
 * text in ccp_entry.s. The MIPS CCP is linked statically with the kernel
 * so we don't need a compressed CCP image; supply zero-size blobs so the
 * help / stty builtins link cleanly (they'll print empty until M4). */
unsigned char ccp_help_lz[1] = { 0 };
unsigned long ccp_help_lz_size = 0;
unsigned char ccp_stty_lz[1] = { 0 };
unsigned long ccp_stty_lz_size = 0;

/* Program loader tail. On 68K this hands control to the freshly loaded
 * image at `cseg` via pgmld_jump.s. The MIPS app-loader (syscall ABI +
 * jalr to TPA entry) lands in a later milestone; for now report failure
 * so BDOS program-load calls return cleanly. */
int pgmld_finish_and_run(u32 cseg, u32 size, const char *cmdtail)
{
    (void)cseg; (void)size; (void)cmdtail;
    return -1;
}

/*------------------------------------------------------------------
 * M4: dynamic app-load demo (function-pointer ABI).
 *   loader memcpy's app bytes to TPA @ 0x0ED40000, then jalrs with
 *   $a0 = bdos_dispatch; app's _app_start trampolines to app_main(bdos);
 *   app returns via `jr $ra`. No 0x80000080 handler needed.
 *------------------------------------------------------------------*/
extern unsigned char _hello_app_start[];
extern unsigned char _hello_app_end[];
extern u32 bdos_dispatch(u32 func, u32 p1, u32 p2);

typedef int (*app_entry_t)(u32 (*)(u32, u32, u32));

int run_embedded_hello(void)
{
    unsigned char *tpa = (unsigned char *)0x0ED40000UL;
    unsigned long sz = (unsigned long)(_hello_app_end - _hello_app_start);
    unsigned long i;

    uart_puts("[loader] hello.bin: "); xputdec(sz);
    uart_puts(" bytes -> TPA @ 0x0ED40000\r\n");

    for (i = 0; i < sz; i++) tpa[i] = _hello_app_start[i];

    app_entry_t entry = (app_entry_t)tpa;
    int rc = entry(bdos_dispatch);

    uart_puts("[loader] app returned, rc="); xputdec((u32)rc);
    uart_puts("\r\n");
    return rc;
}

extern unsigned char _app2_start[];
extern unsigned char _app2_end[];

int run_embedded_app2(void)
{
    unsigned char *tpa = (unsigned char *)0x0ED40000UL;
    unsigned long sz = (unsigned long)(_app2_end - _app2_start);
    unsigned long i;

    uart_puts("[loader] sysinfo.bin: "); xputdec(sz);
    uart_puts(" bytes -> TPA @ 0x0ED40000\r\n");

    for (i = 0; i < sz; i++) tpa[i] = _app2_start[i];

    app_entry_t entry = (app_entry_t)tpa;
    int rc = entry(bdos_dispatch);

    uart_puts("[loader] sysinfo.bin: app returned, rc="); xputdec((u32)rc);
    uart_puts("\r\n");
    return rc;
}

/*------------------------------------------------------------------
 * M5a: load + run an app from the RAM-backed FAT filesystem.
 *   fat_fopen path -> fat_fread bytes into TPA -> jalr with $a0 = bdos.
 *------------------------------------------------------------------*/
static fat_dir_iter_t g_fat_iter;   /* static to avoid stack pressure */

void list_fat_root(void)
{
    fat_dirent_t de;
    char lfn[64];
    int n = 0;

    if (fat_diropen_root(&g_fat_iter) != 0) {
        uart_puts("  list_fat_root: open failed\r\n");
        return;
    }
    uart_puts("  A: directory:\r\n");
    while (fat_dirnext(&g_fat_iter, &de, lfn, (int)sizeof(lfn)) > 0) {
        if (de.attr & FAT_ATTR_VOLID) continue;
        if (de.name[0] == 0xE5 || de.name[0] == 0x00) continue;
        uart_puts("    ");
        /* 8.3 name with embedded space padding */
        for (int i = 0; i < 8; i++) uart_putc(de.name[i] ? de.name[i] : ' ');
        uart_putc(' ');
        for (int i = 8; i < 11; i++) uart_putc(de.name[i] ? de.name[i] : ' ');
        uart_puts("  ");
        xputdec(de.file_size);
        uart_puts(" bytes\r\n");
        n++;
    }
    uart_puts("  ("); xputdec((u32)n); uart_puts(" files)\r\n");
}

int run_app_from_fat(const char *path)
{
    fat_file_t fp;
    unsigned char *tpa = (unsigned char *)0x0ED40000UL;

    if (fat_fopen(path, &fp) != 0) {
        uart_puts("[loader] "); uart_puts(path);
        uart_puts(": fat_fopen failed\r\n");
        return -1;
    }
    u32 sz = fat_fsize(&fp);
    uart_puts("[loader] "); uart_puts(path); uart_puts(": ");
    xputdec(sz); uart_puts(" bytes -> TPA @ 0x0ED40000\r\n");

    int got = fat_fread(&fp, tpa, sz);
    fat_fclose(&fp);
    if (got <= 0) {
        uart_puts("[loader] fat_fread returned ");
        xputdec((u32)got); uart_puts("\r\n");
        return -1;
    }

    app_entry_t entry = (app_entry_t)tpa;
    int rc = entry(bdos_dispatch);

    uart_puts("[loader] "); uart_puts(path);
    uart_puts(": app returned, rc="); xputdec((u32)rc); uart_puts("\r\n");
    return rc;
}
