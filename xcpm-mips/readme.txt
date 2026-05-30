XCP/M-MIPS for the NCD15

This is a CP/M-style operating system recompiled for the
LSI/IDT R3052 MIPS-I CPU that drives the NCD15 X-terminal.

Try:
  DIR             list files
  CAT README.TXT  print this file
  HELLO           function-pointer ABI demo
  SYSCALL         position-independent syscall ABI demo
  SYSINFO         dumps BDOS ident + a hex/checksum of itself
  VER             CCP version (builtin)

Apps load by filename from the FAT12 filesystem embedded
in the kernel ECOFF.  Drop a new app/foo.c into the source
tree, run make, type FOO at the prompt -- it works.
