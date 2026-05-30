/*============================================================================
 * app/cat.c -- print a file to the console (BDOS 111).
 *
 * Usage:  A:\> CAT FOO.TXT
 *
 * Parses the first space-separated token from the command tail, copies
 * it to a local buffer (uppercased), and calls BDOS 111 (d_ext_cat).
 *==========================================================================*/
#include "libxcpm.h"

static int is_space(char c) { return c == ' ' || c == '\t'; }
static char to_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

int app_main(bdos_fn_t bdos, const char *tail)
{
    /* Skip leading whitespace. */
    while (*tail && is_space(*tail)) tail++;
    if (!*tail) {
        xputs_d(bdos, "\r\nUsage: CAT <filename>\r\n$");
        return 1;
    }

    /* Copy the first token, uppercase, up to 32 chars. */
    char name[33];
    int n = 0;
    while (*tail && !is_space(*tail) && n < 32) {
        name[n++] = to_upper(*tail++);
    }
    name[n] = 0;

    xputs(bdos, "\r\n--- ");
    xputs(bdos, name);
    xputs(bdos, " ---\r\n");

    u32 rc = bdos(BDOS_CAT, (u32)name, 0);
    if (rc == 0xFFFFFFFFu) {
        xputs(bdos, "\r\ncat: not found: ");
        xputs(bdos, name);
        xputs(bdos, "\r\n");
        return 2;
    }
    xputs(bdos, "\r\n--- end ---\r\n");
    return 0;
}
