/*============================================================================
 * glue_ncd15.c -- MIPS/NCD15 replacements for the 68K-specific glue that
 * normally lives in main.c + pgmld_jump.s.
 *
 * Milestone 2: warmboot is a stub (no CCP wired yet); the program loader
 * returns failure. Both get real bodies in the CCP / app-loader milestone.
 *==========================================================================*/
#include "xcpm.h"

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
