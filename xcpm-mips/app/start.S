/*============================================================================
 * app/start.S -- MIPS-I crt for a dynamically-loaded XCP/M app.
 *
 * The loader copies the app's flat binary to TPA base, jalrs to offset 0
 * with $a0 = bdos_dispatch (function pointer). This crt jumps to the
 * C entry app_main(bdos), preserving $ra so app_main's `return` reaches
 * the kernel loader cleanly.
 *==========================================================================*/
    .section .text.entry, "ax"
    .set noreorder
    .globl _app_start
    .ent   _app_start
_app_start:
    j   app_main
    nop
    .end _app_start
