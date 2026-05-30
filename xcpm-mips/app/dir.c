/*============================================================================
 * app/dir.c -- list files on A: (BDOS 110).
 *
 * Usage:  A:\> DIR
 *
 * Calls BDOS 110 (d_ext_listroot), which iterates the FAT root and
 * prints each 8.3 name to the console.
 *==========================================================================*/
#include "libxcpm.h"

int app_main(bdos_fn_t bdos, const char *tail)
{
    (void)tail;
    xputs_d(bdos, "\r\nDirectory of A:\r\n--------\r\n$");
    bdos(BDOS_LISTROOT, 0, 0);
    return 0;
}
