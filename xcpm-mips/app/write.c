/*============================================================================
 * app/write.c -- create a file on A: with the given content (BDOS 120).
 *
 * Usage:  A:\> WRITE FOO.TXT some text to save
 *
 * Parses the first space-separated token as the filename (uppercased,
 * up to 32 chars); the rest of the command tail (after one space
 * separator) becomes the file's content. Calls BDOS 120 (d_ext_savefile)
 * with a savebuf{ buf, size } descriptor. The writeable shadow lives
 * between TPA end and stack top so changes persist for this boot
 * session; a reset reverts to the rodata "factory" image.
 *==========================================================================*/
#include "libxcpm.h"

struct savebuf { const u8 *buf; u32 size; };

static int is_space(char c) { return c == ' ' || c == '\t'; }
static char to_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static u32 my_strlen(const char *s)
{
    u32 n = 0;
    while (*s++) n++;
    return n;
}

int app_main(bdos_fn_t bdos, const char *tail)
{
    while (*tail && is_space(*tail)) tail++;
    if (!*tail) {
        xputs_d(bdos, "\r\nUsage: WRITE <filename> <content>\r\n$");
        return 1;
    }

    /* Copy + uppercase the filename. */
    char name[33];
    int n = 0;
    while (*tail && !is_space(*tail) && n < 32) name[n++] = to_upper(*tail++);
    name[n] = 0;

    /* Skip exactly one space separator. */
    if (*tail) tail++;
    const char *content = tail;
    u32 size = my_strlen(content);

    if (size == 0) {
        xputs_d(bdos, "\r\nWRITE: no content given\r\n$");
        return 1;
    }

    struct savebuf sb = { (const u8 *)content, size };
    u32 rc = bdos(120 /* BDOS_SAVEFILE */, (u32)name, (u32)&sb);
    if (rc != 0) {
        xputs(bdos, "\r\nWRITE: failed for ");
        xputs(bdos, name);
        xputs(bdos, "\r\n");
        return 2;
    }
    xputs(bdos, "\r\nWrote ");
    xputdec(bdos, size);
    xputs(bdos, " bytes to ");
    xputs(bdos, name);
    xputs(bdos, "\r\n");
    return 0;
}
