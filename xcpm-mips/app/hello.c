/*============================================================================
 * app/hello.c -- minimal XCP/M-MIPS user app, dynamically loaded into TPA.
 *
 * Uses libxcpm.h for the bdos_fn_t typedef and convenience wrappers.
 * Proves the user-side API: include the header, define app_main, done.
 *==========================================================================*/
#include "libxcpm.h"

int app_main(bdos_fn_t bdos)
{
    static const char msg[] =
        "\r\n--- hello.bin: dynamically-loaded MIPS app speaking ---\r\n"
        "I was just memcpy'd into TPA at 0x0ED40000 and jumped to.\r\n"
        "I'm calling BDOS via the function pointer you passed in via a0.\r\n"
        "Returning to the kernel loader now.\r\n";

    xputs_d(bdos, msg);

    /* Demo libxcpm's numeric helpers: print the kernel's identity magic
     * via BDOS 100, in both hex and decimal, using xputhex8 + xputdec. */
    xputs(bdos, "BDOS 100 ident: 0x");
    xputhex8(bdos, bdos(BDOS_IDENT, 0, 0));
    xputs(bdos, " (");
    xputdec(bdos, bdos(BDOS_IDENT, 0, 0));
    xputs(bdos, " decimal)\r\n");
    return 0;
}
