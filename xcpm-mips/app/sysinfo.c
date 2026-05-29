/*============================================================================
 * app/sysinfo.c -- XCP/M-MIPS environment probe + TPA hex dump.
 *
 * Useful utility primitive: queries the kernel identity (BDOS 100 magic,
 * BDOS 12 version), then hex-dumps the first 64 bytes of the TPA
 * (0x0ED40000) which is this app's own loaded image, and reports an
 * XOR checksum across the dumped range.
 *
 * Demonstrates: BDOS 9 (string), BDOS 2 (single char), BDOS 12, BDOS 100,
 * absolute-address pointer arithmetic, nested loops, and arithmetic.
 *==========================================================================*/

typedef unsigned int   u32;
typedef unsigned char  u8;
typedef u32 (*bdos_fn_t)(u32 func, u32 p1, u32 p2);

static void put_char(bdos_fn_t bdos, char c)
{
    bdos(2, (u32)(u8)c, 0);
}

static void put_str(bdos_fn_t bdos, const char *s)
{
    /* Use BDOS 9 -- caller guarantees s ends with '$'. */
    bdos(9, (u32)s, 0);
}

static void put_hex_nyb(bdos_fn_t bdos, u32 v)
{
    v &= 0xF;
    put_char(bdos, (char)(v < 10 ? ('0' + v) : ('A' + v - 10)));
}

static void put_hex8(bdos_fn_t bdos, u32 v)
{
    put_hex_nyb(bdos, v >> 4);
    put_hex_nyb(bdos, v);
}

static void put_hex32(bdos_fn_t bdos, u32 v)
{
    int i;
    for (i = 28; i >= 0; i -= 4)
        put_hex_nyb(bdos, v >> i);
}

int app_main(bdos_fn_t bdos)
{
    static const char banner[] =
        "\r\n--- sysinfo.bin: XCP/M-MIPS environment probe ---\r\n$";
    static const char lbl_magic[]  = "Kernel ident (BDOS 100) : 0x$";
    static const char lbl_ver[]    = "BDOS version (BDOS  12) : 0x$";
    static const char lbl_tpa[]    = "\r\nTPA hex dump @ 0x0ED40000 (first 64 bytes):\r\n$";
    static const char lbl_cksum[]  = "XOR checksum over 64 bytes: 0x$";
    static const char lbl_done[]   = "\r\nsysinfo done. Returning to loader.\r\n$";
    static const char crlf[]       = "\r\n$";

    u32 magic, ver, cksum;
    const u8 *tpa;
    int row, col;

    put_str(bdos, banner);

    /* --- kernel ident --- */
    magic = bdos(100, 0, 0);
    put_str(bdos, lbl_magic);
    put_hex32(bdos, magic);
    put_str(bdos, crlf);

    /* --- BDOS version --- */
    ver = bdos(12, 0, 0);
    put_str(bdos, lbl_ver);
    put_hex32(bdos, ver);
    put_str(bdos, crlf);

    /* --- hex-dump first 64 bytes of TPA + accumulate XOR cksum --- */
    put_str(bdos, lbl_tpa);
    tpa   = (const u8 *)0x0ED40000u;
    cksum = 0;
    for (row = 0; row < 4; row++) {
        u32 base = (u32)tpa + (u32)(row * 16);
        put_hex32(bdos, base);
        put_char(bdos, ':');
        put_char(bdos, ' ');
        for (col = 0; col < 16; col++) {
            u8 b = tpa[row * 16 + col];
            cksum ^= b;
            put_hex8(bdos, b);
            put_char(bdos, ' ');
        }
        put_char(bdos, '\r');
        put_char(bdos, '\n');
    }

    put_str(bdos, lbl_cksum);
    put_hex32(bdos, cksum);
    put_str(bdos, crlf);

    put_str(bdos, lbl_done);
    return 0;
}
