/*============================================================================
 * main_ncd15.c -- XCP/M entry for the NCD15 (MIPS R3052) port.
 *
 * Milestone 2: bring up the portable OS core (BDOS/BIOS/FAT/ramdisk) on
 * MIPS and exercise it by calling the dispatchers directly (the 68K TRAP
 * shims are replaced by a `syscall` ABI in a later milestone). Proves the
 * shared C compiles and runs unchanged on the R3052.
 *==========================================================================*/
#include "xcpm.h"
#include "bdos.h"
#include "bios.h"

extern u32 _ram_base, _ram_end, _tpa_base, _tpa_end, _stack_top;
extern u32 bdos_dispatch(u32 func, u32 p1, u32 p2);
extern u32 bios_dispatch(u32 func, u32 p1, u32 p2);
extern void bdos_init(void);
extern void bios_init(void);

static void putaddr(const char *label, void *p)
{
    uart_puts(label); uart_puts("0x"); xputhex((u32)p, 8);
}

static void selftest(void)
{
    u32 v;
    uart_puts("Self-test (direct dispatch):\r\n");

    /* BDOS 12: version */
    v = bdos_dispatch(12, 0, 0);
    uart_puts("  BDOS 12 (version)   = 0x"); xputhex(v, 4);
    uart_puts(" (expect 0x0022)\r\n");

    /* BDOS 100: XCP/M identity magic */
    v = bdos_dispatch(100, 0, 0);
    uart_puts("  BDOS 100 (ident)    = 0x"); xputhex(v, 8);
    uart_puts(" (expect 0x58504D4B)\r\n");

    /* BIOS 18: memory region table pointer */
    v = bios_dispatch(18, 0, 0);
    uart_puts("  BIOS 18 (getseg)    = 0x"); xputhex(v, 8); uart_puts("\r\n");

    /* BDOS 9: print '$'-terminated string */
    {
        static char msg[] = "  BDOS 9 (printstr)  : hello via dispatch$";
        bdos_dispatch(9, (u32)msg, 0);
        uart_puts("\r\n");
    }

    /* B: ramdisk round-trip */
    {
        u8 buf[128]; int i;
        for (i = 0; i < 16; i++) buf[i] = (u8)(0xA0 + i);
        for (; i < 128; i++)     buf[i] = 0;
        ramdisk_write(0, buf);
        for (i = 0; i < 128; i++) buf[i] = 0;
        ramdisk_read(0, buf);
        uart_puts("  B: ramdisk ("); xputdec(ramdisk_capacity());
        uart_puts(" sec) head=");
        for (i = 0; i < 8; i++) { xputhex(buf[i], 2); uart_putc(' '); }
        uart_puts("(expect A0 A1 ..)\r\n");
    }
}

void main(void)
{
    uart_init();
    uart_puts("\r\nXCP/M (NCD15/MIPS port) v0.1 -- milestone 2\r\n");
    uart_puts("Board: ncd15  CPU: LSI/IDT R3052 (MIPS-I)\r\n");
    putaddr("RAM  ", &_ram_base); putaddr("..", &_ram_end);
    putaddr("  TPA ", &_tpa_base); uart_puts("\r\n");

    bios_init();
    bdos_init();
    uart_puts("BIOS+BDOS dispatchers initialised.\r\n");

    selftest();

    uart_puts("\r\nM4: dynamic app load demo\r\n");
    extern int run_embedded_hello(void);
    run_embedded_hello();

    uart_puts("\r\nLaunching CCP...\r\n");
    extern void ccp_main(void);
    ccp_main();
    /* CCP exited; fall into warmboot loop. */
    extern void warmboot(void);
    warmboot();
}
