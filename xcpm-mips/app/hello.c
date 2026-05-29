/*============================================================================
 * app/hello.c -- minimal XCP/M-MIPS user app, dynamically loaded into TPA.
 *
 * The kernel-side loader memcpy's this binary's bytes to TPA base
 * (0x0ED40000), then `jalr` with $a0 = bdos_dispatch. We get the function
 * pointer as our first parameter, use it to print a banner via BDOS 9
 * (print '$'-terminated string), and return.
 *
 * Position: linked at the TPA address via app.ld; absolute references in
 * the compiled code therefore resolve correctly once the loader has
 * placed the binary there.
 *==========================================================================*/

typedef unsigned int u32;
typedef u32 (*bdos_fn_t)(u32 func, u32 p1, u32 p2);

int app_main(bdos_fn_t bdos)
{
    static const char msg[] =
        "\r\n--- hello.bin: dynamically-loaded MIPS app speaking ---\r\n"
        "I was just memcpy'd into TPA at 0x0ED40000 and jumped to.\r\n"
        "I'm calling BDOS via the function pointer you passed in via a0.\r\n"
        "Returning to the kernel loader now.\r\n$";

    bdos(9, (u32)msg, 0);
    return 0;
}
