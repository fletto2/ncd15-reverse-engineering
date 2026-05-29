/*============================================================================
 * app/syscall.c -- proves the position-independent syscall ABI.
 *
 * Unlike hello.c / sysinfo.c (which receive the bdos function pointer in
 * $a0), this app uses `bdos_sys()` from libxcpm.h: a real MIPS `syscall`
 * instruction with $v0=func / $a0=p1 / $a1=p2. The kernel handler at
 * 0x80000080 (installed by start.S at boot) marshals those into
 * bdos_dispatch and returns the result in $v0.
 *
 * `bdos` parameter is unused -- proves we don't need it.
 *==========================================================================*/
#include "libxcpm.h"

int app_main(bdos_fn_t bdos)
{
    (void)bdos;

    u32 ver = bdos_sys(BDOS_VERSION, 0, 0);
    u32 id  = bdos_sys(BDOS_IDENT,   0, 0);

    /* Use BDOS 2 (single-char console output) through syscall to print
     * a banner -- no function pointer touched anywhere. */
    static const char banner[] =
        "\r\n--- syscall.bin: position-independent app via 0x80000080 ---\r\n"
        "Got here through a real MIPS `syscall` instruction.\r\n"
        "Kernel handler unwrapped v0/a0/a1 and dispatched to bdos.\r\n"
        "BDOS 12  version : 0x";
    for (const char *p = banner; *p; p++)
        bdos_sys(BDOS_CONOUT, (u32)(u8)*p, 0);
    for (int i = 28; i >= 0; i -= 4) {
        u32 d = (ver >> i) & 0xF;
        bdos_sys(BDOS_CONOUT, (u32)("0123456789ABCDEF"[d]), 0);
    }
    static const char mid[] = "\r\nBDOS 100 ident   : 0x";
    for (const char *p = mid; *p; p++)
        bdos_sys(BDOS_CONOUT, (u32)(u8)*p, 0);
    for (int i = 28; i >= 0; i -= 4) {
        u32 d = (id >> i) & 0xF;
        bdos_sys(BDOS_CONOUT, (u32)("0123456789ABCDEF"[d]), 0);
    }
    static const char tail[] = "\r\nReturning to loader.\r\n";
    for (const char *p = tail; *p; p++)
        bdos_sys(BDOS_CONOUT, (u32)(u8)*p, 0);
    return 0;
}
