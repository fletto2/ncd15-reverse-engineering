/*============================================================================
 * hello_blob.S -- embed app/hello.bin as a rodata blob inside the kernel.
 *
 * The loader in glue_ncd15.c memcpy's _hello_app_start .. _hello_app_end
 * into TPA at 0x0ED40000 and jalr's to offset 0 with $a0 = bdos_dispatch.
 *==========================================================================*/
    .section .rodata.embedded_app, "a"
    .balign 4
    .globl _hello_app_start
_hello_app_start:
    .incbin "app/hello.bin"
    .globl _hello_app_end
_hello_app_end:

    .balign 4
    .globl _app2_start
_app2_start:
    .incbin "app/sysinfo.bin"
    .globl _app2_end
_app2_end:
