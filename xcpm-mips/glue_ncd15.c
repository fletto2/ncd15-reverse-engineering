/*============================================================================
 * glue_ncd15.c -- MIPS/NCD15 replacements for the 68K-specific glue that
 * normally lives in main.c + pgmld_jump.s.
 *
 * Milestone 2: warmboot is a stub (no CCP wired yet); the program loader
 * returns failure. Both get real bodies in the CCP / app-loader milestone.
 *==========================================================================*/
#include "xcpm.h"
#include "fat.h"

/* Called by bdos.c's d_reset / d_ext_loadrun tail via `j warmboot`
 * after an app exits. Re-enter the CCP. The CCP self-decompresses
 * back into TPA each pass, so it's safe to call indefinitely. */
extern void ccp_main(void);
void warmboot(void)
{
    for (;;) {
        ccp_main();
        uart_puts("\r\n[warmboot] CCP exited; restarting\r\n");
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
extern u32 bdos_dispatch(u32 func, u32 p1, u32 p2);
/* App ABI: $a0 = bdos function pointer, $a1 = command tail string.
 * Older apps that take only bdos still work — the tail parameter is
 * just ignored. */
typedef int (*app_entry_t)(u32 (*)(u32, u32, u32), const char *);

/*------------------------------------------------------------------
 * R3052 I-cache invalidate over [base, base+size).
 *   - set CP0 reg 7 bit 13 (cache-isolate — NOT R3000's Status.IsC)
 *   - sw zero in 16-byte strides over the region
 *   - clear the bit
 * On real HW this marks the cache tags invalid so the CPU re-fetches
 * the new bytes we just wrote (`memcpy` or `fat_fread`) instead of
 * executing stale I-cache contents. The NCD15 emulator discards stores
 * while isolation is set, so this is a no-op there but still safe.
 *------------------------------------------------------------------*/
static void icache_flush(unsigned long base, unsigned long size)
{
    unsigned long ccr, on, end;
    end = base + size;
    asm volatile("mfc0 %0, $7\n\t nop" : "=r"(ccr));
    on = ccr | (1ul << 13);
    asm volatile("mtc0 %0, $7\n\t nop\n\t nop\n\t nop" : : "r"(on));
    for (; base < end; base += 16) {
        asm volatile("sw $0, 0(%0)" : : "r"(base) : "memory");
    }
    asm volatile("mtc0 %0, $7\n\t nop\n\t nop\n\t nop" : : "r"(ccr));
}

int pgmld_finish_and_run(u32 cseg, u32 size, const char *cmdtail)
{
    (void)cseg; (void)size; (void)cmdtail;
    return -1;
}

/* Hook called by BDOS 113's .MIP path (try_load_path_mips). Bytes are
 * already at `base` (= TPA_BASE), so we just cast + jalr with
 * $a0 = bdos_dispatch. Returns the app's int rc; warmboot is triggered
 * by d_ext_loadrun's tail. */
int pgmld_run_mips(u32 base, u32 size, const char *cmdtail)
{
    icache_flush((unsigned long)base, size);
    app_entry_t entry = (app_entry_t)base;
    return entry(bdos_dispatch, cmdtail ? cmdtail : "");
}

/*------------------------------------------------------------------
 * M4: dynamic app-load demo (function-pointer ABI).
 *   loader memcpy's app bytes to TPA @ 0x0ED40000, then jalrs with
 *   $a0 = bdos_dispatch; app's _app_start trampolines to app_main(bdos);
 *   app returns via `jr $ra`. No 0x80000080 handler needed.
 *------------------------------------------------------------------*/
extern unsigned char _hello_app_start[];
extern unsigned char _hello_app_end[];

int run_embedded_hello(void)
{
    unsigned char *tpa = (unsigned char *)0x0ED40000UL;
    unsigned long sz = (unsigned long)(_hello_app_end - _hello_app_start);
    unsigned long i;

    uart_puts("[loader] hello.bin: "); xputdec(sz);
    uart_puts(" bytes -> TPA @ 0x0ED40000\r\n");

    for (i = 0; i < sz; i++) tpa[i] = _hello_app_start[i];

    icache_flush((unsigned long)tpa, sz);
    app_entry_t entry = (app_entry_t)tpa;
    int rc = entry(bdos_dispatch, "");

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

    icache_flush((unsigned long)tpa, sz);
    app_entry_t entry = (app_entry_t)tpa;
    int rc = entry(bdos_dispatch, "");

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
    /* fat_dirnext returns 0 on success, 1 at end-of-dir, -1 on error
     * (and silently skips LFN / VOLID / deleted internally). */
    while (fat_dirnext(&g_fat_iter, &de, lfn, (int)sizeof(lfn)) == 0) {
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

    icache_flush((unsigned long)tpa, sz);
    app_entry_t entry = (app_entry_t)tpa;
    int rc = entry(bdos_dispatch, "");

    uart_puts("[loader] "); uart_puts(path);
    uart_puts(": app returned, rc="); xputdec((u32)rc); uart_puts("\r\n");
    return rc;
}
