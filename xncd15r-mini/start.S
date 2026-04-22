/* Entry at 0x0ED00000. Monitor has already inited DUART and jumps here.
 * Set up a stack near the top of the 4 MB image region, call main, halt. */
        .set    noreorder
        .set    mips1
        .section .text.start, "ax"
        .global _start
_start:
        lui     $sp, 0x0ed3            /* sp = 0x0ed3ff00 */
        ori     $sp, $sp, 0xff00
        lui     $gp, 0                 /* we compile -G0; keep gp zero */
        jal     main
        nop
halt:   b       halt
        nop
