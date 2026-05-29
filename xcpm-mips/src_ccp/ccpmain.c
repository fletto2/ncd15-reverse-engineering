/*============================================================================
 * ccpmain.c -- standalone CCP for XCP/M-68K (with history + tab completion)
 *
 * Built as a freestanding .68K image, linked at TPA+0x100, LZSS-compressed
 * and embedded in kernel ROM.  Decompressed back to TPA on every (warm)
 * boot.  Uses ONLY TRAP #2 (BDOS) and TRAP #3 (BIOS) for I/O.
 *==========================================================================*/

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

#define CCP_LINE_MAX    80
#define HISTORY_DEPTH   8           /* lines */
#define ENV_BLOCK_SIZE  256         /* total bytes for all env vars */
#define ENV_MAX_VARS    8           /* slot count */
#define ALIAS_MAX       64          /* tri-vocabulary aliases */

/* Line-editor key constants (used by xnano and read_line). */
#define KEY_UP      0x100
#define KEY_DOWN    0x101
#define KEY_LEFT    0x102
#define KEY_RIGHT   0x103
#define KEY_HOME    0x104
#define KEY_END     0x105
#define KEY_DEL     0x106
#define KEY_C_LEFT  0x107
#define KEY_C_RIGHT 0x108
#define KEY_M_BS    0x109
#define KEY_F7      0x10A
#define KEY_F8      0x10B

static int read_key(void);          /* fwd */

/*--------------------------------------------------------------------------
 * Trap-based BDOS / BIOS calls
 *--------------------------------------------------------------------------*/
#if defined(__mips__)
/* MIPS port: CCP is linked with the kernel in single-mode, so call the
   dispatchers directly rather than going through a syscall trap. FAT-loaded
   apps will use a real syscall ABI; that's a separate path. */
extern u32 bdos_dispatch(u32, u32, u32);
extern u32 bios_dispatch(u32, u32, u32);
static u32 bdos(u32 fn, u32 d1, u32 d2) { return bdos_dispatch(fn, d1, d2); }
static u32 bios(u32 fn, u32 d1, u32 d2) { return bios_dispatch(fn, d1, d2); }
#else
static u32 bdos(u32 fn, u32 d1, u32 d2)
{
    register u32 r0 __asm__("d0") = fn;
    register u32 r1 __asm__("d1") = d1;
    register u32 r2 __asm__("d2") = d2;
    __asm__ volatile ("trap #2"
        : "+d"(r0) : "d"(r1), "d"(r2) : "a0","a1","memory");
    return r0;
}

static u32 bios(u32 fn, u32 d1, u32 d2)
{
    register u32 r0 __asm__("d0") = fn;
    register u32 r1 __asm__("d1") = d1;
    register u32 r2 __asm__("d2") = d2;
    __asm__ volatile ("trap #3"
        : "+d"(r0) : "d"(r1), "d"(r2) : "a0","a1","memory");
    return r0;
}
#endif

/*--------------------------------------------------------------------------
 * I/O helpers
 *--------------------------------------------------------------------------*/
static void putc_(int c)        { bdos(2, (u32)(c & 0xFF), 0); }
static int  getc_raw(void)      { return (int)bios(3, 0, 0) & 0xFF; }

static void puts_(const char *s)
{
    while (*s) {
        if (*s == '\n') putc_('\r');
        putc_((u8)*s);
        s++;
    }
}

static u32 strlen_(const char *s)
{
    u32 n = 0; while (s[n]) n++; return n;
}

static void puthex(u32 v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[9]; int i;
    if (digits == 0) {
        /* Auto-width: just enough digits to represent v (>=1). */
        digits = 1;
        u32 t = v >> 4;
        while (t) { digits++; t >>= 4; }
    }
    if (digits < 1) digits = 1;
    if (digits > 8) digits = 8;
    buf[digits] = 0;
    for (i = digits - 1; i >= 0; i--) { buf[i] = hex[v & 0xF]; v >>= 4; }
    puts_(buf);
}

static void putdec(u32 v)
{
    char buf[12];
    int i = sizeof(buf) - 1;
    buf[i--] = 0;
    if (v == 0) buf[i--] = '0';
    else { while (v && i >= 0) { buf[i--] = '0' + (char)(v % 10); v /= 10; } }
    puts_(&buf[i + 1]);
}

/*--------------------------------------------------------------------------
 * Tiny string helpers
 *--------------------------------------------------------------------------*/
static int isspace_(int c) { return c==' '||c=='\t'; }
static int islower_(int c) { return c>='a' && c<='z'; }
static int toupper_(int c) { return islower_(c) ? c - 32 : c; }

static int strieq(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = toupper_((u8)*a++), cb = toupper_((u8)*b++);
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

static int strieqn(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int ca = toupper_((u8)a[i]), cb = toupper_((u8)b[i]);
        if (!ca || !cb) return ca == cb;
        if (ca != cb) return 0;
    }
    return 1;
}

static void strcpy_(char *d, const char *s)
{ while ((*d++ = *s++) != 0) ; }

/* Centralised usage-message printer.  Each cmd_X passes its own argv
   plus an args-only string; we prepend "usage: " + argv[0] + " " and
   append "\r\n".  Saves repetition of the prefix in every command. */
static void usage_(char **argv, const char *args)
{
    puts_("usage: "); puts_(argv[0]); putc_(' '); puts_(args); puts_("\r\n");
}

/* Print "?prefix: arg: msg\r\n" — used by all the rm/cp/mkdir/etc.
   error paths to factor out the repeated three-puts_ chain.  Pass NULL
   for any field to skip it. */
static void errf_(const char *prefix, const char *arg, const char *msg)
{
    putc_('?');
    if (prefix) puts_(prefix);
    if (arg)    { puts_(": "); puts_(arg); }
    if (msg)    { puts_(": "); puts_(msg); }
    puts_("\r\n");
}

/* Auto-mount the FAT volume on first call; return 1 if ready, 0 if
   the mount failed.  Replaces the duplicated mount-or-fail blocks in
   cmd_ls / cmd_cat / etc. */
static int ensure_mounted_(void)
{
    static int mounted = 0;
    if (!mounted) {
        if (bdos(112, 0, 0) == 0) { puts_("?no volume mounted\r\n"); return 0; }
        mounted = 1;
    }
    return 1;
}

/* Tokenizer with single- and double-quote support.  Inside double quotes,
   \n / \r / \t / \" / \\ are recognized.  Single quotes are literal.
   Both forms of quotes are stripped from the returned token; the rest of
   the token (after a closing quote) continues normally. */
static char *next_token(char **pp)
{
    char *p = *pp;
    while (*p && isspace_((u8)*p)) p++;
    if (!*p) { *pp = p; return 0; }
    char *out = p;          /* write cursor (may equal read cursor) */
    char *tok = p;
    char *r = p;
    while (*r && !isspace_((u8)*r)) {
        char c = *r++;
        if (c == '"') {
            while (*r && *r != '"') {
                if (*r == '\\' && r[1]) {
                    r++;
                    char e = *r++;
                    switch (e) {
                    case 'n': *out++ = '\n'; break;
                    case 'r': *out++ = '\r'; break;
                    case 't': *out++ = '\t'; break;
                    case '\\': *out++ = '\\'; break;
                    case '"': *out++ = '"'; break;
                    default:  *out++ = e; break;
                    }
                } else {
                    *out++ = *r++;
                }
            }
            if (*r == '"') r++;
        } else if (c == '\'') {
            while (*r && *r != '\'') *out++ = *r++;
            if (*r == '\'') r++;
        } else {
            *out++ = c;
        }
    }
    if (*r) { *r = 0; r++; }
    *out = 0;
    *pp = r;
    return tok;
}

static int parse_num(const char *s, u32 *out)
{
    u32 v = 0; int n = 0; int hex = 0;
    if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) { s += 2; hex = 1; }
    while (*s) {
        int c = toupper_((u8)*s++), d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = hex ? (v << 4) | (u32)d : v * 10 + (u32)d;
        if (++n > 10) return 0;
    }
    if (n == 0) return 0;
    *out = v;
    return 1;
}

/*--------------------------------------------------------------------------
 * Built-ins
 *--------------------------------------------------------------------------*/
struct builtin {
    const char *name;
    int (*fn)(int argc, char **argv);
    /* summary string removed — help text lives in compressed blob
       ccp_help_lz, decompressed on demand by cmd_help via BDOS 127. */
};

static const struct builtin builtins[];     /* forward */

static int cmd_help(int argc, char **argv);

static int cmd_ver(int argc, char **argv) { (void)argc;(void)argv;
    puts_("XCP/M-68K v0.1 (compressed CCP)\r\n");
    puts_("BDOS version (BDOS 12) = 0x"); puthex(bdos(12, 0, 0), 4); puts_("\r\n");
    puts_("XCP/M magic   (BDOS 100) = 0x"); puthex(bdos(100, 0, 0), 8); puts_("\r\n");
    return 0;
}

static int cmd_mem(int argc, char **argv) { (void)argc;(void)argv;
    u32 addr = bios(18, 0, 0);
    volatile u16 *p = (volatile u16 *)addr;
    int n = p[0];
    puts_("Memory regions ("); putdec(n); puts_("):\r\n");
    volatile u32 *q = (volatile u32 *)(addr + 2);
    for (int i = 0; i < n; i++) {
        u32 start = q[i*2], length = q[i*2 + 1];
        puts_("  ["); putdec(i); puts_("] 0x"); puthex(start, 6);
        puts_(" .. 0x"); puthex(start + length - 1, 6);
        puts_(" ("); putdec(length); puts_(" bytes)\r\n");
    }
    return 0;
}

static int cmd_cls(int argc, char **argv) { (void)argc;(void)argv;
    puts_("\033[2J\033[H");
    return 0;
}

static int cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) putc_(' ');
        puts_(argv[i]);
    }
    puts_("\r\n");
    return 0;
}

static int cmd_dump(int argc, char **argv) {
    u32 addr;
    if (argc < 2 || !parse_num(argv[1], &addr)) {
        usage_(argv, "<addr> [count]"); return 1;
    }
    u32 count = 64;
    if (argc >= 3) parse_num(argv[2], &count);
    if (count > 256) count = 256;
    volatile u8 *p = (volatile u8 *)addr;
    for (u32 i = 0; i < count; i += 16) {
        puthex(addr + i, 6); puts_(": ");
        for (u32 j = 0; j < 16; j++) {
            if (i + j < count) { puthex(p[i+j], 2); putc_(' '); }
            else                puts_("   ");
        }
        putc_(' ');
        for (u32 j = 0; j < 16 && i + j < count; j++) {
            u8 c = p[i+j];
            putc_((c >= 0x20 && c < 0x7F) ? c : '.');
        }
        puts_("\r\n");
    }
    return 0;
}

static int cmd_peek(int argc, char **argv) {
    u32 addr;
    if (argc < 2 || !parse_num(argv[1], &addr)) {
        usage_(argv, "<addr>"); return 1;
    }
    puts_("0x"); puthex(addr, 6); puts_(":  b=0x");
    puthex(*(volatile u8  *)addr, 2);
    puts_("  w=0x"); puthex(*(volatile u16 *)(addr & ~1u), 4);
    puts_("  l=0x"); puthex(*(volatile u32 *)(addr & ~3u), 8);
    puts_("\r\n");
    return 0;
}

static int cmd_poke(int argc, char **argv) {
    u32 addr, val;
    if (argc < 3 || !parse_num(argv[1], &addr) || !parse_num(argv[2], &val)) {
        usage_(argv, "<addr> <byte>"); return 1;
    }
    *(volatile u8 *)addr = (u8)val;
    puts_("ok\r\n");
    return 0;
}

/* cmd_drive: removed; the "drive" builtin now points at cmd_pwd. */

static int cmd_user(int argc, char **argv) {
    u32 v;
    if (argc < 2) { puts_("user = "); putdec(bdos(32, 0xFF, 0) & 0xFF); puts_("\r\n"); return 0; }
    if (!parse_num(argv[1], &v) || v > 15) {
        puts_("?usage: user 0..15\r\n"); return 1;
    }
    bdos(32, v, 0);
    return 0;
}

static int cmd_history(int argc, char **argv);    /* fwd */
static int cmd_set    (int argc, char **argv);
static int cmd_prompt (int argc, char **argv);
static int cmd_free   (int argc, char **argv);
static int cmd_uname  (int argc, char **argv);
static int cmd_expr   (int argc, char **argv);
static int cmd_seq    (int argc, char **argv);
static int cmd_factor (int argc, char **argv);
static int cmd_printf (int argc, char **argv);
static int cmd_true   (int argc, char **argv);
static int cmd_false  (int argc, char **argv);
static int cmd_yes    (int argc, char **argv);
static int cmd_test   (int argc, char **argv);
static int cmd_crc32  (int argc, char **argv);
static int cmd_base64 (int argc, char **argv);
static int cmd_uue    (int argc, char **argv);
static int cmd_uud    (int argc, char **argv);
static int cmd_stty   (int argc, char **argv);
static int cmd_srec   (int argc, char **argv);
/* cmd_xmodem removed (use base64/uuencode + paste, or srec, or load). */
static int cmd_nano   (int argc, char **argv);
static int cmd_mount  (int argc, char **argv);
static int cmd_ls     (int argc, char **argv);
static int cmd_cat    (int argc, char **argv);
static int cmd_rm     (int argc, char **argv);
static int cmd_cp     (int argc, char **argv);
static int cmd_mv     (int argc, char **argv);
static int cmd_mkdir  (int argc, char **argv);
static int cmd_rmdir  (int argc, char **argv);
static int cmd_cd     (int argc, char **argv);
static int cmd_pwd    (int argc, char **argv);
static int cmd_submit (int argc, char **argv);
static int cmd_save   (int argc, char **argv);
static int cmd_load   (int argc, char **argv);

static int cmd_exit(int argc, char **argv) { (void)argc;(void)argv;
    puts_("warm boot\r\n");
    bdos(0, 0, 0);
    return 0;
}

static const struct builtin builtins[] = {
    { "help",    cmd_help },
    { "?",       cmd_help },
    { "ver",     cmd_ver },
    { "mem",     cmd_mem },
    { "clear",   cmd_cls },
    { "echo",    cmd_echo },
    /* dump dropped — `hexdump` is the Linux-style canonical name. */
    { "peek",    cmd_peek },
    { "poke",    cmd_poke },
    /* "drive" dropped — `pwd` is the Linux-style canonical name. */
    { "user",    cmd_user },
    { "history", cmd_history },
    { "set",     cmd_set },
    { "prompt",  cmd_prompt },
    { "free",    cmd_free },
    { "uname",   cmd_uname },
    { "expr",    cmd_expr },
    { "seq",     cmd_seq },
    { "factor",  cmd_factor },
    { "printf",  cmd_printf },
    { "true",    cmd_true },
    { "false",   cmd_false },
    { "yes",     cmd_yes },
    { "test",    cmd_test },
    { "[",       cmd_test },
    { "crc32",   cmd_crc32 },
    { "hexdump", cmd_dump },
    { "base64",  cmd_base64 },
    { "uuencode",cmd_uue },
    { "uudecode",cmd_uud },
    { "stty",    cmd_stty },
    { "srec",    cmd_srec },
    /* xmodem/rx removed — base64+paste, srec, or load cover transfer. */
    { "nano",    cmd_nano },
    { "edit",    cmd_nano },
    { "mount",   cmd_mount },
    { "ls",      cmd_ls },
    { "cat",     cmd_cat },
    { "rm",      cmd_rm },
    { "cp",      cmd_cp },
    { "mv",      cmd_mv },
    { "mkdir",   cmd_mkdir },
    { "rmdir",   cmd_rmdir },
    { "cd",      cmd_cd },
    { "pwd",     cmd_pwd },
    { "source",  cmd_submit },
    { "save",    cmd_save },
    { "load",    cmd_load },
    { "exit",    cmd_exit },
    { 0, 0, 0 }
};

extern u8  ccp_help_lz[];
extern u32 ccp_help_lz_size;

static int cmd_help(int argc, char **argv) { (void)argc;(void)argv;
    puts_("XCP/M-68K commands:\r\n");
    /* The help text is a compressed "name<TAB>summary\n" blob, embedded
       in the CCP image and decompressed by the kernel via BDOS 127 to
       avoid carrying ~2 KB of help strings uncompressed in .rodata. */
    bdos(127, (u32)ccp_help_lz, ccp_help_lz_size);
    return 0;
}

/*--------------------------------------------------------------------------
 * Small applets (filesystem-independent)
 *--------------------------------------------------------------------------*/
/* Forward references to state defined later: */
int  hist_count;        /* shared with the history-ring code below */
int  env_used;          /* shared with the env-block code below */


static int cmd_free(int argc, char **argv) { (void)argc;(void)argv;
    /* Memory region table tells us TPA size; we know the rest from layout. */
    u32 mr = bios(18, 0, 0);
    volatile u32 *q = (volatile u32 *)(mr + 2);
    u32 tpa_start = q[0], tpa_size = q[1];
    puts_("TPA  : 0x"); puthex(tpa_start, 6);
    puts_(" - 0x"); puthex(tpa_start + tpa_size - 1, 6);
    puts_(" ("); putdec(tpa_size); puts_(" bytes)\r\n");
    puts_("History buffer slots: "); putdec(hist_count);
    puts_("/"); putdec(HISTORY_DEPTH); puts_("\r\n");
    puts_("Env block:            "); putdec(env_used);
    puts_("/"); putdec(ENV_BLOCK_SIZE); puts_(" bytes\r\n");
    return 0;
}

static int cmd_uname(int argc, char **argv) {
    int show_all = (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'a');
    if (show_all) {
        puts_("XCP/M-68K v0.1 m68k pvs2 (compressed CCP, FAT32-native)\r\n");
    } else {
        puts_("XCP/M-68K\r\n");
    }
    return 0;
}

/* Minimal expr: arg1 op arg2.  Integer only. */
static int cmd_expr(int argc, char **argv) {
    if (argc != 4) { usage_(argv, "A op B"); return 1; }
    u32 a, b;
    if (!parse_num(argv[1], &a) || !parse_num(argv[3], &b)) {
        puts_("?bad number\r\n"); return 1;
    }
    u32 r = 0;
    char op = argv[2][0];
    switch (op) {
    case '+': r = a + b; break;
    case '-': r = a - b; break;
    case '*': r = a * b; break;
    case '/': if (b == 0) { puts_("?div by zero\r\n"); return 1; } r = a / b; break;
    case '%': if (b == 0) { puts_("?div by zero\r\n"); return 1; } r = a % b; break;
    case '&': r = a & b; break;
    case '|': r = a | b; break;
    case '^': r = a ^ b; break;
    default: puts_("?op (+,-,*,/,%%,&,|,^)\r\n"); return 1;
    }
    putdec(r); puts_("\r\n");
    return 0;
}

static int cmd_seq(int argc, char **argv) {
    u32 lo = 1, hi;
    if (argc == 2) { if (!parse_num(argv[1], &hi)) { usage_(argv, "[lo] hi"); return 1; } }
    else if (argc == 3) { if (!parse_num(argv[1], &lo) || !parse_num(argv[2], &hi)) {
        usage_(argv, "[lo] hi"); return 1; } }
    else { usage_(argv, "[lo] hi"); return 1; }
    for (u32 i = lo; i <= hi; i++) { putdec(i); puts_("\r\n"); }
    return 0;
}

static int cmd_factor(int argc, char **argv) {
    if (argc < 2) { usage_(argv, "N"); return 1; }
    u32 n;
    if (!parse_num(argv[1], &n) || n < 2) { puts_("?N >= 2\r\n"); return 1; }
    putdec(n); puts_(":");
    u32 d = 2;
    while (n > 1) {
        while (n % d == 0) { putc_(' '); putdec(d); n /= d; }
        d++;
        if (d * d > n && n > 1) { putc_(' '); putdec(n); break; }
    }
    puts_("\r\n");
    return 0;
}

/* Tiny printf: %d %x %X %s %c %% */
static int cmd_printf(int argc, char **argv) {
    if (argc < 2) return 0;
    const char *fmt = argv[1];
    int ai = 2;
    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\\') {
                fmt++;
                if      (*fmt == 'n') putc_('\n');
                else if (*fmt == 'r') putc_('\r');
                else if (*fmt == 't') putc_('\t');
                else if (*fmt == '\\') putc_('\\');
                else                  putc_(*fmt);
                if (*fmt) fmt++;
                continue;
            }
            putc_(*fmt++);
            continue;
        }
        fmt++;
        char c = *fmt++;
        u32 v;
        const char *s;
        switch (c) {
        case 'd': case 'i':
            if (ai >= argc) return 1;
            if (!parse_num(argv[ai], &v)) v = 0;
            ai++; putdec(v); break;
        case 'x':
            if (ai >= argc) return 1;
            if (!parse_num(argv[ai], &v)) v = 0;
            ai++; puthex(v, 0); break;
        case 'X':
            if (ai >= argc) return 1;
            if (!parse_num(argv[ai], &v)) v = 0;
            ai++; puthex(v, 8); break;
        case 's':
            if (ai >= argc) return 1;
            s = argv[ai++]; puts_(s); break;
        case 'c':
            if (ai >= argc) return 1;
            putc_(argv[ai++][0]); break;
        case '%': putc_('%'); break;
        case 0: return 0;
        default:  putc_('%'); putc_(c); break;
        }
    }
    return 0;
}

static int cmd_true (int argc, char **argv) { (void)argc;(void)argv; return 0; }
static int cmd_false(int argc, char **argv) { (void)argc;(void)argv; return 1; }

static int cmd_yes(int argc, char **argv) {
    /* Bounded so it doesn't loop forever in our captured-output sim runs. */
    const char *s = (argc >= 2) ? argv[1] : "y";
    for (int i = 0; i < 8; i++) { puts_(s); puts_("\r\n"); }
    return 0;
}

/* test EXPR : minimum useful subset
   -z S    : true if S is empty
   -n S    : true if S is non-empty
   A = B   : string equal
   A != B  : string not equal
   A -eq B : numeric eq (also -ne -lt -le -gt -ge) */
static int cmd_test(int argc, char **argv) {
    int last = argc - 1;
    if (last >= 1 && argv[last][0] == ']' && argv[last][1] == 0) last--;
    int n = last;     /* number of test args (including position 1..n) */
    if (n == 1) {
        return argv[1][0] == 0 ? 1 : 0;
    }
    if (n == 2) {
        if (strieq(argv[1], "-z")) return argv[2][0] == 0 ? 0 : 1;
        if (strieq(argv[1], "-n")) return argv[2][0] != 0 ? 0 : 1;
        return 1;
    }
    if (n == 3) {
        const char *a = argv[1], *op = argv[2], *b = argv[3];
        if (strieq(op, "=") || strieq(op, "==")) return strieq(a, b) ? 0 : 1;
        if (strieq(op, "!="))                    return strieq(a, b) ? 1 : 0;
        u32 av, bv;
        if (parse_num(a, &av) && parse_num(b, &bv)) {
            if (strieq(op, "-eq")) return (av == bv) ? 0 : 1;
            if (strieq(op, "-ne")) return (av != bv) ? 0 : 1;
            if (strieq(op, "-lt")) return (av <  bv) ? 0 : 1;
            if (strieq(op, "-le")) return (av <= bv) ? 0 : 1;
            if (strieq(op, "-gt")) return (av >  bv) ? 0 : 1;
            if (strieq(op, "-ge")) return (av >= bv) ? 0 : 1;
        }
    }
    return 1;
}

/* CRC32 (IEEE 802.3, polynomial 0xEDB88320, no precomputed table to save ROM) */
static u32 crc32_byte(u32 crc, u8 b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++) {
        crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
    }
    return crc;
}

static int cmd_crc32(int argc, char **argv) {
    if (argc < 2) { usage_(argv, "<ascii-string>"); return 1; }
    u32 crc = 0xFFFFFFFFu;
    for (int a = 1; a < argc; a++) {
        if (a > 1) crc = crc32_byte(crc, ' ');
        const char *p = argv[a];
        while (*p) crc = crc32_byte(crc, (u8)*p++);
    }
    crc ^= 0xFFFFFFFFu;
    puthex(crc, 8); puts_("\r\n");
    return 0;
}

/* Base64 encode/decode of an ASCII argument */
static const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_index(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void b64_encode(const u8 *in, int n)
{
    int i = 0;
    while (i < n) {
        u32 v = (u32)in[i] << 16;
        int rem = n - i;
        if (rem >= 2) v |= (u32)in[i+1] << 8;
        if (rem >= 3) v |= (u32)in[i+2];
        putc_(b64chars[(v >> 18) & 0x3F]);
        putc_(b64chars[(v >> 12) & 0x3F]);
        putc_(rem >= 2 ? b64chars[(v >> 6) & 0x3F] : '=');
        putc_(rem >= 3 ? b64chars[ v       & 0x3F] : '=');
        i += 3;
    }
    puts_("\r\n");
}

static void b64_decode(const char *in, int n)
{
    int i = 0;
    while (i + 3 < n + 4 && in[i]) {
        int a = b64_index((u8)in[i]);
        int b = (i+1 < n) ? b64_index((u8)in[i+1]) : -1;
        int c = (i+2 < n) ? (in[i+2] == '=' ? -2 : b64_index((u8)in[i+2])) : -2;
        int d = (i+3 < n) ? (in[i+3] == '=' ? -2 : b64_index((u8)in[i+3])) : -2;
        if (a < 0 || b < 0) break;
        putc_((char)((a << 2) | (b >> 4)));
        if (c >= 0) putc_((char)(((b & 0xF) << 4) | (c >> 2)));
        if (d >= 0) putc_((char)(((c & 0x3) << 6) |  d      ));
        i += 4;
    }
    puts_("\r\n");
}

static int cmd_base64(int argc, char **argv) {
    int decode = 0; int ai = 1;
    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'd') { decode = 1; ai = 2; }
    if (ai >= argc) { usage_(argv, "[-d] <ascii>"); return 1; }
    const char *s = argv[ai];
    int n = strlen_(s);
    if (decode) b64_decode(s, n);
    else        b64_encode((const u8 *)s, n);
    return 0;
}

/* uuencode / uudecode of a single ASCII argument */
static int cmd_uue(int argc, char **argv) {
    if (argc < 2) { usage_(argv, "<ascii>"); return 1; }
    const char *s = argv[1];
    int n = strlen_(s);
    if (n > 45) n = 45;     /* canonical uuencode line */
    putc_((char)(n + 0x20));
    int i = 0;
    while (i < n) {
        int c1 = (u8)s[i];
        int c2 = (i + 1 < n) ? (u8)s[i + 1] : 0;
        int c3 = (i + 2 < n) ? (u8)s[i + 2] : 0;
        int b1 = c1 >> 2;
        int b2 = ((c1 & 3) << 4) | (c2 >> 4);
        int b3 = ((c2 & 0xF) << 2) | (c3 >> 6);
        int b4 = c3 & 0x3F;
        putc_((char)(b1 ? b1 + 0x20 : 0x60));
        putc_((char)(b2 ? b2 + 0x20 : 0x60));
        putc_((char)(b3 ? b3 + 0x20 : 0x60));
        putc_((char)(b4 ? b4 + 0x20 : 0x60));
        i += 3;
    }
    puts_("\r\n");
    return 0;
}

static int cmd_uud(int argc, char **argv) {
    if (argc < 2) { usage_(argv, "<line>"); return 1; }
    const char *s = argv[1];
    if (!*s) return 1;
    int n = (u8)*s++ - 0x20;
    if (n < 0 || n > 45) { puts_("?bad length\r\n"); return 1; }
    int written = 0;
    while (written < n && s[0]) {
        int b1 = ((u8)s[0] - 0x20) & 0x3F;
        int b2 = s[1] ? ((u8)s[1] - 0x20) & 0x3F : 0;
        int b3 = s[2] ? ((u8)s[2] - 0x20) & 0x3F : 0;
        int b4 = s[3] ? ((u8)s[3] - 0x20) & 0x3F : 0;
        if (written < n) putc_((char)((b1 << 2) | (b2 >> 4)));
        written++;
        if (written < n) putc_((char)((b2 << 4) | (b3 >> 2)));
        written++;
        if (written < n) putc_((char)((b3 << 6) |  b4      ));
        written++;
        s += 4;
    }
    puts_("\r\n");
    return 0;
}

/*--------------------------------------------------------------------------
 * stty -- query/show console parameters.  Phase 1: read-only summary.
 * The DUART is initialized at 9600 8N1 by the kernel and not reconfigured
 * here.  Future phases can implement `stty 19200`, `stty raw -echo` etc.
 *--------------------------------------------------------------------------*/
extern u8  ccp_stty_lz[];
extern u32 ccp_stty_lz_size;

static int cmd_stty(int argc, char **argv) {
    if (argc >= 2 && strieq(argv[1], "-a")) {
        bdos(127, (u32)ccp_stty_lz, ccp_stty_lz_size);
    } else {
        puts_("speed 9600 baud; line = 0\r\n");
    }
    return 0;
}

/*--------------------------------------------------------------------------
 * srec -- Motorola S-record loader/dumper.
 *
 * Receive: read lines from console; parse S0/S1/S2/S3 data records and
 *          deposit bytes at their encoded addresses.  S5 = count check;
 *          S7/S8/S9 = entry address.  Verifies per-record checksum.
 *
 * Send:    dump a memory range as S1 records to the console.
 *--------------------------------------------------------------------------*/
static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Read one line of up to max chars from the console, terminated by CR/LF. */
static int srec_read_line(char *buf, int max)
{
    int n = 0;
    for (;;) {
        int c = getc_raw();
        if (c == '\r' || c == '\n') {
            if (n > 0) { buf[n] = 0; return n; }
            continue;
        }
        if (c == 0x03) { return -1; }   /* Ctrl-C */
        if (n < max - 1) buf[n++] = (char)c;
    }
}

/* Read two hex digits at line[*pp], advance *pp by 2.  Returns the
   byte value, or -1 on bad hex.  Shared between S-record address-,
   data-, count- and checksum-parsing paths. */
static int srec_byte(const char *line, int *pp)
{
    int hi = hex_nibble(line[*pp]);
    int lo = hex_nibble(line[*pp + 1]);
    *pp += 2;
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

/* Read alen hex chars (alen even) as a big-endian address. */
static int srec_addr(const char *line, int *pp, int alen, u32 *out)
{
    u32 a = 0;
    for (int i = 0; i < alen; i += 2) {
        int b = srec_byte(line, pp);
        if (b < 0) return 0;
        a = (a << 8) | (u32)b;
    }
    *out = a;
    return 1;
}

/* Emit one byte as 2 hex chars and accumulate into cksum.  Used by
   the -w sender for byte-count, address bytes and data bytes alike. */
static void srec_putb(u8 b, u8 *cksum)
{
    puthex(b, 2);
    *cksum += b;
}

static int cmd_srec(int argc, char **argv) {
    int do_write = (argc >= 2 && strieq(argv[1], "-w"));
    int do_exec  = 0;
    if (do_write) {
        u32 addr, len;
        if (argc < 4 || !parse_num(argv[2], &addr) || !parse_num(argv[3], &len)) {
            usage_(argv, "-w addr len"); return 1;
        }
        const u8 *src = (const u8 *)addr;
        for (u32 off = 0; off < len; ) {
            u32 chunk = (len - off > 16) ? 16 : (len - off);
            u32 a = addr + off;
            /* S2: byte_count = data + 3-byte addr + 1-byte cksum */
            puts_("S2");
            u8 cksum = 0;
            srec_putb((u8)(chunk + 4),    &cksum);
            srec_putb((u8)((a >> 16) & 0xFF), &cksum);
            srec_putb((u8)((a >>  8) & 0xFF), &cksum);
            srec_putb((u8)( a        & 0xFF), &cksum);
            for (u32 i = 0; i < chunk; i++) srec_putb(src[off + i], &cksum);
            puthex((u8)~cksum, 2);
            puts_("\r\n");
            off += chunk;
        }
        puts_("S804000000FB\r\n");
        return 0;
    }

    if (argc >= 2 && strieq(argv[1], "--exec")) do_exec = 1;
    puts_("Send S-records (Ctrl-C to abort):\r\n");
    char line[256];
    u32 entry = 0;
    int got_entry = 0;
    for (;;) {
        int n = srec_read_line(line, sizeof(line));
        if (n < 0) { puts_("\r\n?aborted\r\n"); return 1; }
        if (line[0] != 'S') { puts_("?expected S-record\r\n"); continue; }
        int type = line[1];

        if (type == '7' || type == '8' || type == '9') {
            int alen = (type == '7') ? 8 : (type == '8') ? 6 : 4;
            int p = 4;
            if (!srec_addr(line, &p, alen, &entry)) { puts_("?bad hex\r\n"); break; }
            got_entry = 1;
            puts_("end record, entry=0x"); puthex(entry, 6); puts_("\r\n");
            break;
        }
        if (type < '0' || type > '5') continue;     /* skip S4/S5 etc. */

        /* Data records (S1/S2/S3) */
        int p = 2;
        int byte_count = srec_byte(line, &p);
        if (byte_count < 1) { puts_("?bad count\r\n"); continue; }
        int alen = (type == '1') ? 4 : (type == '2') ? 6 : (type == '3') ? 8 : 0;
        if (alen == 0) continue;
        u32 a;
        if (!srec_addr(line, &p, alen, &a)) { puts_("?bad hex\r\n"); continue; }
        u8 cksum = (u8)byte_count + (u8)((a >> 24) & 0xFF) + (u8)((a >> 16) & 0xFF)
                                  + (u8)((a >>  8) & 0xFF) + (u8)(a & 0xFF);
        int data_len = byte_count - (alen / 2) - 1;
        u8 *dst = (u8 *)a;
        int err = 0;
        for (int i = 0; i < data_len; i++) {
            int b = srec_byte(line, &p);
            if (b < 0) { puts_("?bad hex\r\n"); err = 1; break; }
            *dst++ = (u8)b; cksum += (u8)b;
        }
        if (err) continue;
        int cklo = srec_byte(line, &p);
        if (cklo < 0 || (u8)~cksum != (u8)cklo) {
            puts_("?cksum @ 0x"); puthex(a, 6); puts_("\r\n");
        }
    }
    if (got_entry && do_exec && entry != 0) {
        puts_("transferring control...\r\n");
        ((void (*)(void))entry)();
    }
    return 0;
}

/* xmodem dropped — use base64/uuencode + paste over a serial line for
   small binary transfers; for larger payloads, srec or load via FAT. */

/*--------------------------------------------------------------------------
 * xnano -- in-memory text editor.
 * Heavy; not built when CCP_NO_NANO is defined (used on tight RAM boards
 * to fit the CCP into TPA; nano can be loaded as a .68K from disk).
 *--------------------------------------------------------------------------*/
#ifndef CCP_NO_NANO
static int cmd_nano(int argc, char **argv) {
    if (argc < 2) { usage_(argv, "<addr> [size]"); return 1; }
    u32 addr, size = 4096;
    if (!parse_num(argv[1], &addr)) { puts_("?bad addr\r\n"); return 1; }
    if (argc >= 3) parse_num(argv[2], &size);
    if (size > 32768) size = 32768;     /* sanity */

    u8 *buf = (u8 *)addr;

    /* Determine initial length: position of first 0 or full size. */
    u32 len = 0;
    while (len < size && buf[len]) len++;
    u32 cur = 0;

    /* Clear screen, draw status bar at top */
    puts_("\033[2J\033[H");
    puts_("\033[7m  XCP/M-68K nano  ^X exit  ^G help  buf=0x");
    puthex(addr, 6);
    puts_(" size=");
    putdec(size);
    puts_("\033[0m\r\n");
    int header_lines = 1;

    /* Render the buffer below the header. */
    puts_("\033[2;1H");
    for (u32 i = 0; i < len; i++) {
        if (buf[i] == '\n') puts_("\r\n");
        else if (buf[i] >= 0x20 && buf[i] < 0x7F) putc_(buf[i]);
        else putc_('.');
    }
    /* Move cursor to current position. */
    u32 row = header_lines + 1, col = 1;
    for (u32 i = 0; i < cur; i++) {
        if (buf[i] == '\n') { row++; col = 1; } else col++;
    }
    puts_("\033["); putdec(row); puts_(";"); putdec(col); puts_("H");

    for (;;) {
        int k = read_key();
        switch (k) {
        case 24:        /* Ctrl-X */
            puts_("\033[24;1H\r\n");
            puts_("nano: ");
            putdec(len);
            puts_(" bytes left at 0x");
            puthex(addr, 6);
            puts_("\r\n");
            return 0;

        case 7:         /* Ctrl-G */
            puts_("\033[24;1H\033[K");
            puts_("\033[7m^X exit  ^G help  arrows move  ^A home  ^E end\033[0m");
            break;

        case '\r':
        case '\n':
            if (len < size) {
                for (u32 i = len; i > cur; i--) buf[i] = buf[i - 1];
                buf[cur++] = '\n';
                len++;
                puts_("\033[2J\033[H");
                puts_("\033[7m  XCP/M-68K nano  ^X exit  ^G help\033[0m\r\n");
                puts_("\033[2;1H");
                for (u32 i = 0; i < len; i++) {
                    if (buf[i] == '\n') puts_("\r\n");
                    else putc_(buf[i] >= 0x20 ? buf[i] : '.');
                }
                row = header_lines + 1; col = 1;
                for (u32 i = 0; i < cur; i++)
                    if (buf[i] == '\n') { row++; col = 1; } else col++;
                puts_("\033["); putdec(row); puts_(";"); putdec(col); puts_("H");
            }
            break;

        case 0x08: case 0x7F:
            if (cur > 0) {
                for (u32 i = cur - 1; i < len - 1; i++) buf[i] = buf[i + 1];
                len--; cur--;
                puts_("\033[2J\033[H");
                puts_("\033[7m  XCP/M-68K nano  ^X exit  ^G help\033[0m\r\n");
                puts_("\033[2;1H");
                for (u32 i = 0; i < len; i++) {
                    if (buf[i] == '\n') puts_("\r\n");
                    else putc_(buf[i] >= 0x20 ? buf[i] : '.');
                }
                row = header_lines + 1; col = 1;
                for (u32 i = 0; i < cur; i++)
                    if (buf[i] == '\n') { row++; col = 1; } else col++;
                puts_("\033["); putdec(row); puts_(";"); putdec(col); puts_("H");
            }
            break;

        case KEY_LEFT:
            if (cur > 0) { cur--; puts_("\033[D"); }
            break;
        case KEY_RIGHT:
            if (cur < len) { cur++; puts_("\033[C"); }
            break;
        case KEY_UP:
            puts_("\033[A");
            /* ... */
            break;
        case KEY_DOWN:
            puts_("\033[B");
            break;
        case 1: case KEY_HOME:
            puts_("\r");
            break;

        default:
            if (k >= 0x20 && k < 0x7F && len < size - 1) {
                for (u32 i = len; i > cur; i--) buf[i] = buf[i - 1];
                buf[cur++] = (u8)k;
                len++;
                putc_((char)k);
                /* Repaint to handle insert-not-overwrite */
                puts_("\033[s");        /* save cursor */
                for (u32 i = cur; i < len; i++) putc_(buf[i] >= 0x20 ? buf[i] : '.');
                puts_("\033[u");        /* restore cursor */
            }
            break;
        }
    }
}
#else
static int cmd_nano(int argc, char **argv) {
    (void)argc; (void)argv;
    puts_("?nano not available in this build (load EDIT.68K from disk)\r\n");
    return 1;
}
#endif

/*--------------------------------------------------------------------------
 * FAT front-ends -- thin wrappers around BDOS extension functions 110-112.
 *--------------------------------------------------------------------------*/
static int cmd_mount(int argc, char **argv) { (void)argc;(void)argv;
    u32 t = bdos(112, 0, 0);
    if (t == 0) { puts_("?mount failed\r\n"); return 1; }
    puts_("FAT"); putdec(t); puts_(" volume mounted\r\n");
    return 0;
}

static int cmd_ls(int argc, char **argv) { (void)argc;(void)argv;
    if (!ensure_mounted_()) return 1;
    u32 r = bdos(110, 0, 0);
    if (r == 0xFFFFFFFFu) { errf_("ls", 0, "failed"); return 1; }
    puts_("("); putdec(r); puts_(" entries)\r\n");
    return 0;
}

static int cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        /* No filename — read stdin until 0x1A or NUL (the BIOS-conin
           hook returns 0x1A on input-redirect EOF).  BDOS 1 echoes
           the byte itself, so cat just has to keep pulling. */
        for (;;) {
            int c = (int)bdos(1, 0, 0) & 0xFF;
            if (c == 0x1A || c == 0) break;
        }
        return 0;
    }
    if (!ensure_mounted_()) return 1;
    int err = 0;
    for (int a = 1; a < argc; a++) {
        if (argc > 2) { puts_("==> "); puts_(argv[a]); puts_(" <==\r\n"); }
        if (bdos(111, (u32)argv[a], 0) == 0xFFFFFFFFu) {
            errf_("cat", argv[a], "not found"); err = 1;
        }
    }
    return err;
}

/*--------------------------------------------------------------------------
 * Wildcard / glob expansion against the FAT root.
 *
 * Translates a CP/M-style glob ("*.TXT") to a series of dirents via
 * BDOS 17/18 (Search First/Next).  The 11-char pattern is built by
 * expanding '*' to '?'-fill then truncating; '?' passes through.
 *
 * Note: this also expands DOS7-style and Linux-style wildcards in argv,
 * since they all use '*' and '?' the same way.  Path traversal in
 * patterns (e.g. "DOCS/*.TXT") is not handled in this slice.
 *--------------------------------------------------------------------------*/
static int glob_pattern_to_11(const char *pat, char out11[11])
{
    /* Build space-padded 11-char pattern from "name.ext" form. */
    int saw_wild = 0;
    int i, j = 0;
    for (i = 0; i < 11; i++) out11[i] = ' ';
    /* Base portion */
    while (*pat && *pat != '.' && j < 8) {
        char c = *pat++;
        if (c == '*') { while (j < 8) out11[j++] = '?'; saw_wild = 1; break; }
        if (c == '?') { saw_wild = 1; out11[j++] = '?'; }
        else if (c >= 'a' && c <= 'z') out11[j++] = c - 32;
        else                            out11[j++] = c;
    }
    /* Skip rest of base if it overran 8 chars */
    while (*pat && *pat != '.') pat++;
    if (*pat == '.') pat++;
    j = 8;
    while (*pat && j < 11) {
        char c = *pat++;
        if (c == '*') { while (j < 11) out11[j++] = '?'; saw_wild = 1; break; }
        if (c == '?') { saw_wild = 1; out11[j++] = '?'; }
        else if (c >= 'a' && c <= 'z') out11[j++] = c - 32;
        else                            out11[j++] = c;
    }
    return saw_wild;
}

/* Glob `pattern` against the FAT root, writing matching SFNs back-to-back
   into `out_buf` (each NUL-terminated).  Stores pointers into `out_argv`
   up to `max_args`.  Returns the number of matches. */
static int glob_expand(const char *pattern, char *out_buf, int out_buf_size,
                       char **out_argv, int max_args)
{
    char pat11[11];
    if (!glob_pattern_to_11(pattern, pat11)) {
        /* No wildcard -- pass through unchanged. */
        int n = 0;
        while (pattern[n] && n < out_buf_size - 1) { out_buf[n] = pattern[n]; n++; }
        out_buf[n] = 0;
        if (max_args >= 1) out_argv[0] = out_buf;
        return 1;
    }

    /* Set up an FCB in DMA region with the pattern, call BDOS 17 / 18. */
    u8 fcb[36];
    u8 dma[128];
    int i;
    for (i = 0; i < 36; i++) fcb[i] = 0;
    for (i = 0; i < 11; i++) fcb[1 + i] = (u8)pat11[i];
    bdos(26, (u32)dma, 0);              /* Set DMA */

    int matches = 0;
    int boff = 0;
    u32 r = bdos(17, (u32)fcb, 0);      /* Search First */
    while (r == 0 && matches < max_args) {
        /* Build "BASE.EXT" from dma[1..11] */
        char *dst = &out_buf[boff];
        int n = 0;
        for (i = 0; i < 8 && dma[1 + i] != ' '; i++)
            if (boff + n < out_buf_size - 1) dst[n++] = (char)dma[1 + i];
        int has_ext = 0;
        for (i = 0; i < 3; i++) if (dma[9 + i] != ' ') { has_ext = 1; break; }
        if (has_ext) {
            if (boff + n < out_buf_size - 1) dst[n++] = '.';
            for (i = 0; i < 3 && dma[9 + i] != ' '; i++)
                if (boff + n < out_buf_size - 1) dst[n++] = (char)dma[9 + i];
        }
        if (boff + n < out_buf_size - 1) dst[n++] = 0;
        out_argv[matches++] = dst;
        boff += n;
        r = bdos(18, (u32)fcb, 0);      /* Search Next */
    }
    return matches;
}

/*--------------------------------------------------------------------------
 * Filename helpers: build a 36-byte FCB from "FOO.TXT" form.
 *--------------------------------------------------------------------------*/
/* Pack a "FOO.TXT"-form name into 11 bytes at dst (8 base + 3 ext,
   space-padded, uppercased). Used by build_fcb (rm/cp) and cmd_mv. */
static void fcb_pack_name(u8 *dst, const char *name)
{
    int j;
    for (j = 0; j < 11; j++) dst[j] = ' ';
    j = 0;
    while (*name && *name != '.' && j < 8) {
        char c = *name++;
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[j++] = (u8)c;
    }
    if (*name == '.') name++;
    j = 8;
    while (*name && j < 11) {
        char c = *name++;
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[j++] = (u8)c;
    }
}

static void build_fcb(u8 *fcb, const char *name)
{
    int i;
    for (i = 0; i < 36; i++) fcb[i] = 0;
    fcb_pack_name(&fcb[1], name);
}

static int cmd_rm(int argc, char **argv) {
    if (argc < 2) { usage_(argv, "<name>"); return 1; }
    int err = 0;
    for (int a = 1; a < argc; a++) {
        u8 fcb[36];
        build_fcb(fcb, argv[a]);
        u32 r = bdos(19, (u32)fcb, 0);
        if (r != 0) {
            errf_("rm", argv[a], "not found");
            err = 1;
        }
    }
    return err;
}

static int cmd_cp(int argc, char **argv) {
    if (argc < 3) { usage_(argv, "<src> <dst>"); return 1; }
    if (!ensure_mounted_()) return 1;
    /* BDOS 123 does a byte-precise FAT copy in the kernel; the old
       FCB-based path here used to round to 128-byte CP/M records. */
    u32 r = bdos(123, (u32)argv[1], (u32)argv[2]);
    if (r == 0xFFFFFFFFu) { errf_("cp", argv[1], "failed"); return 1; }
    puts_("ok\r\n");
    return 0;
}

static int cmd_mv(int argc, char **argv) {
    if (argc < 3) { usage_(argv, "<old> <new>"); return 1; }
    /* BDOS 23 expects a 32-byte FCB-pair: old name at +1..+11, new name
       at +17..+27.  Both names go through fcb_pack_name. */
    u8 buf[36];
    for (int i = 0; i < 36; i++) buf[i] = 0;
    fcb_pack_name(&buf[1],  argv[1]);
    fcb_pack_name(&buf[17], argv[2]);
    if (bdos(23, (u32)buf, 0) != 0) { errf_("mv", 0, "failed"); return 1; }
    puts_("ok\r\n");
    return 0;
}

static int cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { usage_(argv, "<name>"); return 1; }
    u32 r = bdos(118, (u32)argv[1], 0);
    if (r != 0) { errf_("mkdir", argv[1], "failed"); return 1; }
    puts_("ok\r\n");
    return 0;
}

static int cmd_rmdir(int argc, char **argv) {
    if (argc < 2) { usage_(argv, "<name>"); return 1; }
    u32 r = bdos(119, (u32)argv[1], 0);
    if (r == 1)  { errf_("rmdir", argv[1], "not empty"); return 1; }
    if (r != 0)  { errf_("rmdir", argv[1], "failed");    return 1; }
    puts_("ok\r\n");
    return 0;
}

static int cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    u32 d = bdos(25, 0, 0);
    u32 u = bdos(32, 0xFF, 0) & 0xFF;
    static char cwd[40];
    bdos(125, (u32)cwd, sizeof(cwd));
    char buf[8]; int n = 0;
    buf[n++] = (char)('A' + (d & 0x0F));
    buf[n++] = ':';
    if (u > 0) {
        if (u >= 10) buf[n++] = '0' + (u / 10);
        buf[n++] = '0' + (u % 10);
    }
    for (int i = 0; i < n; i++) bdos(2, (u32)buf[i], 0);
    if (cwd[0]) puts_(cwd); else puts_("/");
    puts_("\r\n");
    return 0;
}

/* Try interpreting a single token as drive switch ("B:") or user-area
   change (numeric).  Returns 1 if handled, 0 otherwise (caller should
   treat it as a subdir path). */
static int cd_try_drive_or_user(const char *t)
{
    if (t[0] && t[1] == ':' && t[2] == 0) {
        int c = toupper_((u8)t[0]);
        if (c < 'A' || c > 'P') { puts_("?cd: bad drive\r\n"); return 1; }
        bdos(14, (u32)(c - 'A'), 0);
        return 1;
    }
    /* All-digits => user-area. Don't claim "12" if file "12" exists. */
    const char *p = t;
    int u = 0;
    while (*p >= '0' && *p <= '9') { u = u * 10 + (*p++ - '0'); }
    if (*p == 0 && u >= 0 && u <= 15) {
        bdos(32, (u32)u, 0);
        return 1;
    }
    return 0;
}

static int cmd_cd(int argc, char **argv) {
    if (argc < 2) return cmd_pwd(0, 0);
    /* Copy argv[1] to a local — bdos(124) walks into the FAT layer,
       which can corrupt CCP BSS via kernel-stack overflow on tight RAM
       (see CLAUDE.md).  We need the name to print error messages with. */
    char name[40];
    int  i = 0;
    for (; argv[1][i] && i < (int)sizeof(name) - 1; i++) name[i] = argv[1][i];
    name[i] = 0;
    if (cd_try_drive_or_user(name)) return 0;
    /* Print the path BEFORE the bdos call — kernel-stack overflow can
       stomp CCP BSS/stack during fat_diropen_path on a missing target,
       so reading argv[1] (or a local copy) afterwards yields garbage. */
    if (bdos(124, (u32)name, 0) != 0) {
        puts_("?cd: no such directory\r\n");
        return 1;
    }
    return 0;
}

/*--------------------------------------------------------------------------
 * History ring
 *--------------------------------------------------------------------------*/
static char hist_buf[HISTORY_DEPTH][CCP_LINE_MAX];
/* hist_count declared earlier (shared with cmd_free); not static so the
   forward-decl resolves. */
static int  hist_head;      /* next slot to write */

static void history_add(const char *line)
{
    if (!line[0]) return;
    /* Skip duplicate of most recent entry. */
    if (hist_count > 0) {
        int last = (hist_head + HISTORY_DEPTH - 1) % HISTORY_DEPTH;
        if (strieq(hist_buf[last], line)) return;
    }
    int n = strlen_(line);
    if (n >= CCP_LINE_MAX) n = CCP_LINE_MAX - 1;
    int i;
    for (i = 0; i < n; i++) hist_buf[hist_head][i] = line[i];
    hist_buf[hist_head][n] = 0;
    hist_head = (hist_head + 1) % HISTORY_DEPTH;
    if (hist_count < HISTORY_DEPTH) hist_count++;
}

/* Fetch entry at "back-index" 1..hist_count (1 = most recent). */
static const char *history_get(int back_idx)
{
    if (back_idx < 1 || back_idx > hist_count) return 0;
    int slot = (hist_head + HISTORY_DEPTH - back_idx) % HISTORY_DEPTH;
    return hist_buf[slot];
}

static int cmd_history(int argc, char **argv) { (void)argc;(void)argv;
    if (hist_count == 0) { puts_("(no history)\r\n"); return 0; }
    for (int i = hist_count; i >= 1; i--) {
        const char *p = history_get(i);
        putdec(hist_count - i + 1); puts_("  "); puts_(p); puts_("\r\n");
    }
    return 0;
}

/*--------------------------------------------------------------------------
 * Tri-vocabulary alias table (CP/M / DOS7 / Linux -> canonical builtin)
 * §16.1 -- complete naming overlap so users can type any vocabulary.
 *--------------------------------------------------------------------------*/
struct alias { const char *from; const char *to; };

/* True aliases: name → existing builtin that does the same thing. */
static const struct alias aliases[] = {
    { "env",    "set"  },
    { "export", "set"  },
    { "quit",   "exit" },
    { "bye",    "exit" },
    { "logout", "exit" },
    { 0, 0 }
};

/* Names that still need write-side or path-traversal filesystem features
   not in phase-1.  Read-only ops (ls, cat, dir, type) are wired through
   BDOS 110/111 below. */
static const char *fs_needed[] = {
    "rm", "cp", "mv", "mkdir", "rmdir",
    "cd", "pwd", "which", "find", "grep", "more", "less", "head", "tail",
    "wc", "ERA", "ERASE", "DEL", "REN", "RENAME",
    "COPY", "MOVE", "XCOPY", "MD", "RD", "CD",
    "CHDIR", "WHERE", "FIND", "FINDSTR", "MORE",
    0
};

static int needs_fs(const char *name)
{
    for (const char **p = fs_needed; *p; p++) if (strieq(name, *p)) return 1;
    return 0;
}

static const char *resolve_alias(const char *name)
{
    for (const struct alias *a = aliases; a->from; a++) {
        if (strieq(name, a->from)) return a->to;
    }
    return name;
}

/*--------------------------------------------------------------------------
 * Environment block: simple flat "NAME=VALUE\0NAME=VALUE\0...\0\0"
 *--------------------------------------------------------------------------*/
static char env_block[ENV_BLOCK_SIZE];
/* env_used declared earlier (shared with cmd_free). */

static char *env_find(const char *name)
{
    int nlen = strlen_(name);
    int i = 0;
    while (i < env_used) {
        char *e = &env_block[i];
        int j = 0;
        while (e[j] && e[j] != '=') j++;
        if (j == nlen && strieqn(e, name, nlen) && e[j] == '=')
            return e;
        i += strlen_(e) + 1;
    }
    return 0;
}

static const char *env_get(const char *name)
{
    char *e = env_find(name);
    if (!e) return 0;
    while (*e && *e != '=') e++;
    if (*e == '=') e++;
    return e;
}

static int env_set(const char *name, const char *value)
{
    char *e = env_find(name);
    int nlen = strlen_(name);
    int vlen = strlen_(value);
    int new_total = nlen + 1 + vlen + 1;     /* "name=value\0" */

    if (e) {
        /* Compact out the old entry, then append. */
        char *next = e;
        while (*next) next++;
        next++;     /* past trailing \0 */
        int old_len = next - e;
        int rem = env_used - (e - env_block) - old_len;
        for (int i = 0; i < rem; i++) e[i] = e[old_len + i];
        env_used -= old_len;
    }
    if (env_used + new_total >= ENV_BLOCK_SIZE) return 0;
    char *dst = &env_block[env_used];
    int i;
    for (i = 0; i < nlen; i++) dst[i] = name[i];
    dst[nlen] = '=';
    for (i = 0; i < vlen; i++) dst[nlen + 1 + i] = value[i];
    dst[nlen + 1 + vlen] = 0;
    env_used += new_total;
    return 1;
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 2) {
        /* List */
        int i = 0;
        while (i < env_used) {
            puts_(&env_block[i]);
            puts_("\r\n");
            i += strlen_(&env_block[i]) + 1;
        }
        if (env_used == 0) puts_("(no environment variables)\r\n");
        return 0;
    }
    /* Forms: SET NAME=VALUE   or   SET NAME VALUE   or   export NAME=VALUE */
    char buf[128];
    int n = 0;
    for (int a = 1; a < argc && n < (int)sizeof(buf) - 1; a++) {
        if (a > 1) buf[n++] = ' ';
        const char *p = argv[a];
        while (*p && n < (int)sizeof(buf) - 1) buf[n++] = *p++;
    }
    buf[n] = 0;

    /* Split at first '=' or whitespace */
    int eq = -1;
    for (int i = 0; i < n; i++) if (buf[i] == '=') { eq = i; break; }
    if (eq < 0) {
        /* No '=' -- treat as query */
        const char *v = env_get(buf);
        if (v) { puts_(buf); puts_("="); puts_(v); puts_("\r\n"); }
        else   { puts_(buf); puts_(" not set\r\n"); }
        return 0;
    }
    buf[eq] = 0;
    if (!env_set(buf, &buf[eq + 1])) {
        puts_("?env block full\r\n");
        return 1;
    }
    return 0;
}

/*--------------------------------------------------------------------------
 * Tab completion -- command-name prefix only (phase 1)
 *--------------------------------------------------------------------------*/
/* If `prefix` (length n) matches exactly one builtin's name, copy it
   into `out` and return 1.  If multiple match, copy the longest common
   prefix and return -count (so caller can list).  If none, return 0. */
static int complete_command(const char *prefix, int n, char *out)
{
    int matches = 0;
    const char *first = 0;
    /* Count matches and find first */
    for (const struct builtin *b = builtins; b->name; b++) {
        if (strieqn(b->name, prefix, n)) {
            if (matches++ == 0) first = b->name;
        }
    }
    if (matches == 0) return 0;
    if (matches == 1) {
        strcpy_(out, first);
        return 1;
    }
    /* Find longest common prefix among all matches. */
    int common = strlen_(first);
    for (const struct builtin *b = builtins; b->name; b++) {
        if (!strieqn(b->name, prefix, n)) continue;
        int j = 0;
        while (j < common && b->name[j] && first[j] &&
               toupper_((u8)b->name[j]) == toupper_((u8)first[j])) j++;
        common = j;
    }
    int i;
    for (i = 0; i < common; i++) out[i] = first[i];
    out[common] = 0;
    return -matches;
}

/* Complete a filename prefix against the FAT root.  Behavior parallels
   complete_command: returns 1 + writes name to `out` if exactly one
   match; returns -count and writes longest common prefix if multiple;
   returns 0 if no matches. */
static int complete_filename(const char *prefix, int n, char *out, int outsz)
{
    /* Build an "<prefix>*" pattern (CP/M 8.3 form). */
    char pat11[11];
    int i;
    /* Pre-fill with '?' (= match any), then plug in known prefix chars */
    for (i = 0; i < 11; i++) pat11[i] = '?';
    /* Re-derive the 8.3 pattern from the prefix string.
       Find dot if present. */
    int dot = -1;
    for (i = 0; i < n; i++) if (prefix[i] == '.') { dot = i; break; }
    /* Base portion */
    int base_len = (dot < 0) ? n : dot;
    if (base_len > 8) base_len = 8;
    for (i = 0; i < base_len; i++) {
        char c = prefix[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        pat11[i] = c;
    }
    /* Pad rest of base with '?' (anything matches) -- already filled */
    /* Ext portion */
    if (dot >= 0) {
        int ext_chars = n - dot - 1;
        if (ext_chars > 3) ext_chars = 3;
        for (i = 0; i < ext_chars; i++) {
            char c = prefix[dot + 1 + i];
            if (c >= 'a' && c <= 'z') c -= 32;
            pat11[8 + i] = c;
        }
        /* Beyond what user typed: ? wildcard already there */
    }

    /* Walk root via BDOS 17/18 with our pattern. */
    u8 fcb[36];
    u8 dma[128];
    for (i = 0; i < 36; i++) fcb[i] = 0;
    for (i = 0; i < 11; i++) fcb[1 + i] = (u8)pat11[i];
    bdos(26, (u32)dma, 0);              /* Set DMA */

    int matches = 0;
    char first[16];
    int common = 0;
    char common_buf[16];
    int saw_first = 0;

    u32 r = bdos(17, (u32)fcb, 0);
    while (r == 0) {
        /* Build "BASE.EXT" from dma[1..11] */
        char cand[16]; int cn = 0;
        for (i = 0; i < 8 && dma[1 + i] != ' '; i++) cand[cn++] = (char)dma[1 + i];
        int has_ext = 0;
        for (i = 0; i < 3; i++) if (dma[9 + i] != ' ') { has_ext = 1; break; }
        if (has_ext) {
            cand[cn++] = '.';
            for (i = 0; i < 3 && dma[9 + i] != ' '; i++) cand[cn++] = (char)dma[9 + i];
        }
        cand[cn] = 0;

        if (!saw_first) {
            for (i = 0; i < cn; i++) first[i] = cand[i];
            first[cn] = 0;
            for (i = 0; i < cn; i++) common_buf[i] = cand[i];
            common = cn;
            saw_first = 1;
        } else {
            /* Reduce common to longest prefix shared with cand */
            int j = 0;
            while (j < common && cand[j] && common_buf[j] == cand[j]) j++;
            common = j;
        }
        matches++;
        r = bdos(18, (u32)fcb, 0);
    }

    if (matches == 0) return 0;
    if (matches == 1) {
        int j;
        for (j = 0; j < (int)strlen_(first) && j < outsz - 1; j++) out[j] = first[j];
        out[j] = 0;
        return 1;
    }
    /* Multiple: copy common prefix */
    int j;
    for (j = 0; j < common && j < outsz - 1; j++) out[j] = common_buf[j];
    out[j] = 0;
    return -matches;
}

static void list_filename_matches(const char *prefix, int n)
{
    /* Same as complete_filename but list each match. */
    char pat11[11];
    int i;
    for (i = 0; i < 11; i++) pat11[i] = '?';
    int dot = -1;
    for (i = 0; i < n; i++) if (prefix[i] == '.') { dot = i; break; }
    int base_len = (dot < 0) ? n : dot;
    if (base_len > 8) base_len = 8;
    for (i = 0; i < base_len; i++) {
        char c = prefix[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        pat11[i] = c;
    }
    if (dot >= 0) {
        int ext_chars = n - dot - 1;
        if (ext_chars > 3) ext_chars = 3;
        for (i = 0; i < ext_chars; i++) {
            char c = prefix[dot + 1 + i];
            if (c >= 'a' && c <= 'z') c -= 32;
            pat11[8 + i] = c;
        }
    }
    u8 fcb[36], dma[128];
    for (i = 0; i < 36; i++) fcb[i] = 0;
    for (i = 0; i < 11; i++) fcb[1 + i] = (u8)pat11[i];
    bdos(26, (u32)dma, 0);

    puts_("\r\n");
    int col = 0;
    u32 r = bdos(17, (u32)fcb, 0);
    while (r == 0) {
        for (i = 0; i < 8 && dma[1 + i] != ' '; i++) putc_(dma[1 + i]);
        int has_ext = 0;
        for (i = 0; i < 3; i++) if (dma[9 + i] != ' ') { has_ext = 1; break; }
        int printed = 0;
        for (i = 0; i < 8 && dma[1 + i] != ' '; i++) printed++;
        if (has_ext) {
            putc_('.'); printed++;
            for (i = 0; i < 3 && dma[9 + i] != ' '; i++) { putc_(dma[9 + i]); printed++; }
        }
        while (printed < 14) { putc_(' '); printed++; }
        if (++col >= 5) { puts_("\r\n"); col = 0; }
        r = bdos(18, (u32)fcb, 0);
    }
    if (col > 0) puts_("\r\n");
}

static void list_command_matches(const char *prefix, int n)
{
    puts_("\r\n");
    int col = 0;
    for (const struct builtin *b = builtins; b->name; b++) {
        if (!strieqn(b->name, prefix, n)) continue;
        puts_(b->name);
        int pad = 14 - strlen_(b->name);
        while (pad-- > 0) putc_(' ');
        if (++col >= 5) { puts_("\r\n"); col = 0; }
    }
    if (col > 0) puts_("\r\n");
}

/*--------------------------------------------------------------------------
 * Line editor
 *--------------------------------------------------------------------------*/
/* Single-slot kill ring shared across the line editor. */
static char kill_buf[CCP_LINE_MAX];
static int  kill_len;

static int read_key(void)
{
    int c = getc_raw();
    if (c == 0x1B) {
        int c2 = getc_raw();
        if (c2 == 0x7F || c2 == 0x08) return KEY_M_BS;
        if (c2 != '[' && c2 != 'O') return 0x1B;
        int c3 = getc_raw();
        switch (c3) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case '1': {
            /* Multiplexed forms after ESC[1:
                 ESC[1;5C = Ctrl-Right    ESC[1;5D = Ctrl-Left
                 ESC[1;5H = Ctrl-Home     ESC[1;5F = Ctrl-End
                 ESC[1~   = Home (vt100)
                 ESC[18~  = F7            ESC[19~  = F8 */
            int c4 = getc_raw();
            if (c4 == ';') {
                int c5 = getc_raw(); int c6 = getc_raw();
                (void)c5;
                if (c6 == 'C') return KEY_C_RIGHT;
                if (c6 == 'D') return KEY_C_LEFT;
                if (c6 == 'H') return KEY_HOME;
                if (c6 == 'F') return KEY_END;
                return 0;
            }
            if (c4 == '~') return KEY_HOME;
            if (c4 == '8' && getc_raw() == '~') return KEY_F7;
            if (c4 == '9' && getc_raw() == '~') return KEY_F8;
            return 0;
        }
        case '3':
            if (getc_raw() == '~') return KEY_DEL;
            return 0;
        case '4':
            if (getc_raw() == '~') return KEY_END;
            return 0;
        case '5': case '6':
            if (getc_raw() == '~') return 0;    /* PgUp/PgDn ignored */
            return 0;
        case '2':
            if (getc_raw() == '~') return 0;
            return 0;
        }
        return 0;
    }
    return c;
}

/*--------------------------------------------------------------------------
 * F7 popup -- DOS-style numbered history list, pick one and return it.
 *--------------------------------------------------------------------------*/
static int f7_popup(char *out)
{
    if (hist_count == 0) return 0;
    puts_("\r\n");
    for (int i = 1; i <= hist_count; i++) {
        const char *h = history_get(i);
        if (!h) continue;
        if (i < 10) putc_(' ');
        putdec(i); puts_(": "); puts_(h); puts_("\r\n");
    }
    puts_("Pick (1-"); putdec(hist_count); puts_(") or ESC: ");
    char num[8]; int ni = 0;
    for (;;) {
        int c = getc_raw();
        if (c == 0x1B) { puts_("\r\n"); return 0; }
        if (c == '\r' || c == '\n') break;
        if (c >= '0' && c <= '9' && ni < 7) { num[ni++] = (char)c; putc_((char)c); }
        else if ((c == 0x08 || c == 0x7F) && ni > 0) {
            ni--; puts_("\b \b");
        }
    }
    num[ni] = 0;
    if (ni == 0) { puts_("\r\n"); return 0; }
    u32 idx; parse_num(num, &idx);
    if (idx < 1 || idx > (u32)hist_count) { puts_("\r\n"); return 0; }
    /* "1" in popup = oldest; we want history_get(hist_count - idx + 1) */
    const char *h = history_get(hist_count - (int)idx + 1);
    if (!h) { puts_("\r\n"); return 0; }
    int n = strlen_(h);
    if (n >= CCP_LINE_MAX) n = CCP_LINE_MAX - 1;
    int i; for (i = 0; i < n; i++) out[i] = h[i];
    out[n] = 0;
    puts_("\r\n");
    return n;
}

/*--------------------------------------------------------------------------
 * Ctrl-R reverse-incremental search.  Returns the chosen history line in
 * `out` and its length (0 if cancelled).
 *--------------------------------------------------------------------------*/
static int contains_(const char *hay, const char *needle, int nlen)
{
    if (nlen == 0) return 1;
    int hlen = strlen_(hay);
    for (int i = 0; i + nlen <= hlen; i++) {
        if (strieqn(hay + i, needle, nlen)) return 1;
    }
    return 0;
}

static int reverse_isearch(char *out)
{
    char query[CCP_LINE_MAX]; int qlen = 0;
    int back = 1;       /* search starting from this back-index */
    const char *match = 0;
    int match_idx = 0;

    auto_redraw:
    /* Render: \r ESC[K (bck-i-search)`query': match */
    putc_('\r'); puts_("\033[K");
    puts_("(reverse-i-search)`");
    for (int i = 0; i < qlen; i++) putc_(query[i]);
    puts_("': ");
    if (match) puts_(match);

    int c = getc_raw();
    if (c == 0x1B || c == 0x07) {
        puts_("\r\n"); return 0;
    }
    if (c == '\r' || c == '\n') {
        puts_("\r\n");
        if (!match) return 0;
        int n = strlen_(match);
        if (n >= CCP_LINE_MAX) n = CCP_LINE_MAX - 1;
        for (int i = 0; i < n; i++) out[i] = match[i];
        out[n] = 0;
        return n;
    }
    if (c == 18) {      /* Ctrl-R: search older */
        back++;
    } else if (c == 0x08 || c == 0x7F) {
        if (qlen > 0) qlen--;
    } else if (c >= 0x20 && c < 0x7F && qlen < CCP_LINE_MAX - 1) {
        query[qlen++] = (char)c;
        back = 1;
    } else if (c == 3) { puts_("\r\n"); return 0; }

    /* Re-search */
    match = 0; match_idx = 0;
    for (int i = back; i <= hist_count; i++) {
        const char *h = history_get(i);
        if (h && contains_(h, query, qlen)) { match = h; match_idx = i; break; }
    }
    (void)match_idx;
    goto auto_redraw;
}

/* Repaint the line area: \r, prompt, full line text, ESC[K, then move
   cursor back to the right column. */
static void repaint(const char *prompt, int prompt_len,
                    const char *line, int len, int cursor)
{
    putc_('\r');
    puts_(prompt);
    for (int i = 0; i < len; i++) putc_(line[i]);
    puts_("\033[K");
    int back = len - cursor;
    if (back > 0) { puts_("\033["); putdec(back); puts_("D"); }
    (void)prompt_len;
}

static int read_line(const char *prompt, int prompt_len, char *line)
{
    int len = 0, cursor = 0;
    int hist_pos = 0;          /* 0 = current new line, 1..N = history */
    char saved_current[CCP_LINE_MAX];     /* what was on the line before Up */
    int  saved_len = 0, saved_cursor = 0;
    int  last_was_tab = 0;

    line[0] = 0;
    repaint(prompt, prompt_len, line, len, cursor);

    for (;;) {
        int k = read_key();
        int is_tab = 0;

        switch (k) {
        case '\r':
        case '\n':
            putc_('\r'); putc_('\n');
            line[len] = 0;
            return len;

        case 0x08:      /* Ctrl-H / BS */
        case 0x7F:      /* DEL key on Unix terminals */
            if (cursor > 0) {
                for (int i = cursor; i < len; i++) line[i - 1] = line[i];
                len--; cursor--;
                line[len] = 0;
                repaint(prompt, prompt_len, line, len, cursor);
            }
            break;

        case KEY_DEL:
            if (cursor < len) {
                for (int i = cursor; i < len - 1; i++) line[i] = line[i + 1];
                len--;
                line[len] = 0;
                repaint(prompt, prompt_len, line, len, cursor);
            }
            break;

        case KEY_LEFT:
            if (cursor > 0) { cursor--; puts_("\033[D"); }
            break;
        case KEY_RIGHT:
            if (cursor < len) { cursor++; puts_("\033[C"); }
            break;
        case 1:         /* Ctrl-A */
        case KEY_HOME:
            if (cursor > 0) {
                puts_("\033["); putdec(cursor); puts_("D");
                cursor = 0;
            }
            break;
        case 5:         /* Ctrl-E */
        case KEY_END:
            if (cursor < len) {
                puts_("\033["); putdec(len - cursor); puts_("C");
                cursor = len;
            }
            break;

        case 11:        /* Ctrl-K: kill to end */
            if (cursor < len) {
                kill_len = len - cursor;
                for (int i = 0; i < kill_len; i++) kill_buf[i] = line[cursor + i];
                kill_buf[kill_len] = 0;
                len = cursor;
                line[len] = 0;
                puts_("\033[K");
            }
            break;
        case 21:        /* Ctrl-U: kill to start */
            if (cursor > 0) {
                kill_len = cursor;
                for (int i = 0; i < kill_len; i++) kill_buf[i] = line[i];
                kill_buf[kill_len] = 0;
                int rem = len - cursor;
                for (int i = 0; i < rem; i++) line[i] = line[cursor + i];
                len = rem; cursor = 0;
                line[len] = 0;
                repaint(prompt, prompt_len, line, len, cursor);
            }
            break;
        case 25:        /* Ctrl-Y: yank kill buffer at cursor */
            if (kill_len > 0 && len + kill_len < CCP_LINE_MAX - 1) {
                /* shift tail right */
                for (int i = len + kill_len - 1; i >= cursor + kill_len; i--)
                    line[i] = line[i - kill_len];
                for (int i = 0; i < kill_len; i++)
                    line[cursor + i] = kill_buf[i];
                len += kill_len;
                cursor += kill_len;
                line[len] = 0;
                repaint(prompt, prompt_len, line, len, cursor);
            }
            break;
        case 18: {      /* Ctrl-R: reverse-incremental search */
            char picked[CCP_LINE_MAX];
            int n = reverse_isearch(picked);
            if (n > 0) {
                int i; for (i = 0; i < n; i++) line[i] = picked[i];
                line[n] = 0; len = n; cursor = n;
            }
            repaint(prompt, prompt_len, line, len, cursor);
            break;
        }
        case KEY_F7: {
            char picked[CCP_LINE_MAX];
            int n = f7_popup(picked);
            if (n > 0) {
                int i; for (i = 0; i < n; i++) line[i] = picked[i];
                line[n] = 0; len = n; cursor = n;
            }
            repaint(prompt, prompt_len, line, len, cursor);
            break;
        }
        case KEY_F8: {
            /* Walk backward through history for the next entry whose
               prefix matches the current line. */
            int p_back = 1;
            const char *m = 0;
            for (int i = 1; i <= hist_count; i++) {
                const char *h = history_get(i);
                if (h && strieqn(h, line, len)) {
                    /* skip earlier matches if user has hit F8 repeatedly --
                       we don't track that across calls; first match wins. */
                    m = h; (void)p_back; break;
                }
            }
            if (m) {
                int n = strlen_(m);
                if (n >= CCP_LINE_MAX) n = CCP_LINE_MAX - 1;
                int i; for (i = 0; i < n; i++) line[i] = m[i];
                line[n] = 0; len = n; cursor = n;
                repaint(prompt, prompt_len, line, len, cursor);
            }
            break;
        }
        case KEY_C_LEFT: {
            int j = cursor;
            while (j > 0 && isspace_((u8)line[j - 1])) j--;
            while (j > 0 && !isspace_((u8)line[j - 1])) j--;
            if (j != cursor) {
                puts_("\033["); putdec(cursor - j); puts_("D");
                cursor = j;
            }
            break;
        }
        case KEY_C_RIGHT: {
            int j = cursor;
            while (j < len && isspace_((u8)line[j])) j++;
            while (j < len && !isspace_((u8)line[j])) j++;
            if (j != cursor) {
                puts_("\033["); putdec(j - cursor); puts_("C");
                cursor = j;
            }
            break;
        }
        case KEY_M_BS:
        case 23:        /* Ctrl-W: kill previous word */
            if (cursor > 0) {
                int j = cursor;
                while (j > 0 && isspace_((u8)line[j - 1])) j--;
                while (j > 0 && !isspace_((u8)line[j - 1])) j--;
                int killed = cursor - j;
                kill_len = killed;
                for (int i = 0; i < killed; i++) kill_buf[i] = line[j + i];
                kill_buf[kill_len] = 0;
                int tail = len - cursor;
                for (int i = 0; i < tail; i++) line[j + i] = line[cursor + i];
                len -= killed; cursor = j;
                line[len] = 0;
                repaint(prompt, prompt_len, line, len, cursor);
            }
            break;
        case 12:        /* Ctrl-L: clear screen, redraw */
            puts_("\033[2J\033[H");
            repaint(prompt, prompt_len, line, len, cursor);
            break;
        case 3:         /* Ctrl-C: cancel line */
            puts_("^C\r\n");
            line[0] = 0;
            return 0;
        case 4:         /* Ctrl-D: warm boot if line is empty */
            if (len == 0) {
                puts_("^D\r\n");
                bdos(0, 0, 0);
            }
            break;

        case KEY_UP:
            if (hist_pos < hist_count) {
                if (hist_pos == 0) {
                    /* save the current edit buffer */
                    int i;
                    for (i = 0; i < len; i++) saved_current[i] = line[i];
                    saved_current[len] = 0;
                    saved_len = len;
                    saved_cursor = cursor;
                }
                hist_pos++;
                const char *h = history_get(hist_pos);
                int n = strlen_(h);
                if (n >= CCP_LINE_MAX) n = CCP_LINE_MAX - 1;
                int i;
                for (i = 0; i < n; i++) line[i] = h[i];
                line[n] = 0;
                len = n; cursor = n;
                repaint(prompt, prompt_len, line, len, cursor);
            }
            break;
        case KEY_DOWN:
            if (hist_pos > 0) {
                hist_pos--;
                if (hist_pos == 0) {
                    int i;
                    for (i = 0; i < saved_len; i++) line[i] = saved_current[i];
                    line[saved_len] = 0;
                    len = saved_len; cursor = saved_cursor;
                } else {
                    const char *h = history_get(hist_pos);
                    int n = strlen_(h);
                    if (n >= CCP_LINE_MAX) n = CCP_LINE_MAX - 1;
                    int i;
                    for (i = 0; i < n; i++) line[i] = h[i];
                    line[n] = 0;
                    len = n; cursor = n;
                }
                repaint(prompt, prompt_len, line, len, cursor);
            }
            break;

        case '\t': {        /* Tab: complete current token */
            is_tab = 1;
            /* Find the token boundaries under cursor. */
            int tok_start = cursor;
            while (tok_start > 0 && !isspace_((u8)line[tok_start - 1])) tok_start--;
            int tok_end = tok_start;
            while (tok_end < cursor && !isspace_((u8)line[tok_end])) tok_end++;
            if (tok_end != cursor) break;  /* cursor not at end of token */

            char prefix_buf[CCP_LINE_MAX];
            int n = tok_end - tok_start;
            int i;
            for (i = 0; i < n; i++) prefix_buf[i] = line[tok_start + i];
            prefix_buf[n] = 0;

            /* If tok_start == 0, this is the command position -> complete
               builtin names.  Otherwise it's a filename argument. */
            int is_first_token = (tok_start == 0);
            char comp[CCP_LINE_MAX];
            int rc;
            if (is_first_token) {
                rc = complete_command(prefix_buf, n, comp);
            } else {
                rc = complete_filename(prefix_buf, n, comp, sizeof(comp));
            }

            if (rc == 0) {
                /* No match — beep */
                putc_(0x07);
            } else if (rc == 1) {
                /* Single match: replace token and append space */
                int compn = strlen_(comp);
                int tail = len - tok_end;
                int new_len = tok_start + compn + 1 + tail;
                if (new_len >= CCP_LINE_MAX) break;
                for (i = tail - 1; i >= 0; i--)
                    line[tok_start + compn + 1 + i] = line[tok_end + i];
                for (i = 0; i < compn; i++) line[tok_start + i] = comp[i];
                line[tok_start + compn] = ' ';
                len = new_len;
                cursor = tok_start + compn + 1;
                line[len] = 0;
                repaint(prompt, prompt_len, line, len, cursor);
            } else {
                /* Multiple: extend to common prefix; on second tab, list. */
                int compn = strlen_(comp);
                if (compn > n) {
                    int tail = len - tok_end;
                    for (i = tail - 1; i >= 0; i--)
                        line[tok_start + compn + i] = line[tok_end + i];
                    for (i = 0; i < compn; i++) line[tok_start + i] = comp[i];
                    len = tok_start + compn + tail;
                    cursor = tok_start + compn;
                    line[len] = 0;
                    repaint(prompt, prompt_len, line, len, cursor);
                } else if (last_was_tab) {
                    if (is_first_token)
                        list_command_matches(prefix_buf, n);
                    else
                        list_filename_matches(prefix_buf, n);
                    repaint(prompt, prompt_len, line, len, cursor);
                } else {
                    putc_(0x07);
                }
            }
            break;
        }

        default:
            if (k >= 0x20 && k < 0x7F && len < CCP_LINE_MAX - 1) {
                /* Insert printable char at cursor */
                for (int i = len; i > cursor; i--) line[i] = line[i - 1];
                line[cursor++] = (char)k;
                len++;
                line[len] = 0;
                repaint(prompt, prompt_len, line, len, cursor);
            }
            break;
        }

        last_was_tab = is_tab;
        hist_pos = hist_pos;        /* (no-op, reminder we don't reset on edit) */
    }
}

/*--------------------------------------------------------------------------
 * History substitution: !!, !N, !prefix
 *--------------------------------------------------------------------------*/
static int hist_substitute(char *line)
{
    if (line[0] != '!' || line[1] == 0) return 0;
    const char *src = 0;
    if (line[1] == '!') {
        src = history_get(1);
    } else if (line[1] >= '0' && line[1] <= '9') {
        u32 v = 0; const char *p = &line[1];
        while (*p >= '0' && *p <= '9') { v = v * 10 + (u32)(*p - '0'); p++; }
        /* Convert from "1 = oldest" display index to back-index. */
        if (v >= 1 && v <= (u32)hist_count) {
            src = history_get(hist_count - (int)v + 1);
        }
    } else {
        /* !prefix: most recent line starting with prefix */
        const char *prefix = &line[1];
        int plen = strlen_(prefix);
        for (int i = 1; i <= hist_count; i++) {
            const char *h = history_get(i);
            if (h && strieqn(h, prefix, plen)) { src = h; break; }
        }
    }
    if (!src) {
        puts_("?no match\r\n");
        return -1;
    }
    /* Replace line in place; print the substituted command first. */
    int n = strlen_(src);
    if (n >= CCP_LINE_MAX) n = CCP_LINE_MAX - 1;
    int i;
    for (i = 0; i < n; i++) line[i] = src[i];
    line[n] = 0;
    puts_(line); puts_("\r\n");
    return 1;
}

/*--------------------------------------------------------------------------
 * Variable expansion: $NAME and %NAME% on a single token.
 * Returns the expanded token (in `out`) or the original on no expansion.
 *--------------------------------------------------------------------------*/
static void expand_token(const char *in, char *out, int max)
{
    int oi = 0; const char *p = in;
    while (*p && oi < max - 1) {
        char c = *p;
        if (c == '$' && p[1] && (islower_(p[1]) || (p[1]>='A'&&p[1]<='Z') || p[1]=='_')) {
            /* $NAME ... */
            p++;
            char name[32]; int ni = 0;
            while (*p && (islower_(*p) || (*p>='A'&&*p<='Z') ||
                          (*p>='0'&&*p<='9') || *p=='_') && ni < 31) {
                name[ni++] = *p++;
            }
            name[ni] = 0;
            const char *v = env_get(name);
            if (v) while (*v && oi < max - 1) out[oi++] = *v++;
        } else if (c == '%' && p[1]) {
            /* %NAME% -- only if NAME is alnum/underscore and a closing
               '%' is found before any non-name char. */
            const char *q = p + 1;
            while (*q && *q != '%') {
                int qc = *q;
                if (!(islower_(qc) || (qc>='A'&&qc<='Z')
                       || (qc>='0'&&qc<='9') || qc=='_')) break;
                q++;
            }
            if (*q == '%' && q > p + 1) {
                char name[32]; int ni = 0;
                const char *r = p + 1;
                while (r < q && ni < 31) name[ni++] = *r++;
                name[ni] = 0;
                const char *v = env_get(name);
                if (v) while (*v && oi < max - 1) out[oi++] = *v++;
                p = q + 1;
                continue;
            }
            out[oi++] = c; p++;
        } else {
            out[oi++] = c; p++;
        }
    }
    out[oi] = 0;
}

/*--------------------------------------------------------------------------
 * `prompt` builtin
 *--------------------------------------------------------------------------*/
static int cmd_prompt(int argc, char **argv)
{
    if (argc < 2) {
        const char *p = env_get("PROMPT");
        if (p) { puts_("PROMPT="); puts_(p); puts_("\r\n"); }
        else   { puts_("PROMPT not set (default = $P$G$S)\r\n"); }
        return 0;
    }
    /* Reassemble argv[1..] with spaces */
    char buf[64];
    int n = 0;
    for (int a = 1; a < argc && n < (int)sizeof(buf) - 1; a++) {
        if (a > 1) buf[n++] = ' ';
        const char *p = argv[a];
        while (*p && n < (int)sizeof(buf) - 1) buf[n++] = *p++;
    }
    buf[n] = 0;
    env_set("PROMPT", buf);
    return 0;
}

/*--------------------------------------------------------------------------
 * Drive switch + command dispatch
 *--------------------------------------------------------------------------*/
static int try_drive_switch(const char *tok)
{
    if (!tok || !tok[0] || tok[1] != ':' || tok[2] != 0) return 0;
    int c = toupper_((u8)tok[0]);
    if (c < 'A' || c > 'P') return 0;
    bdos(14, (u32)(c - 'A'), 0);
    return 1;
}

static int has_wildcard(const char *s) {
    while (*s) { if (*s == '*' || *s == '?') return 1; s++; }
    return 0;
}

/* Argv backing store used when the line has a redirect.  Lives in
   the tail of the kernel-side lzss_window scratch (BDOS 122) — well
   below where deep FAT-trap overflows reach. */
#define ARGV_SAFE_SIZE  (CCP_LINE_MAX * 2)
#define argv_safe_room  ARGV_SAFE_SIZE
static char *argv_safe_ptr(void)
{
    struct { u8 *buf; u32 size; } si;
    bdos(122, (u32)&si, 0);
    /* Tail of the 4 KB scratch — submit owns 0..2047, inrd 2048..
       4095-160; we get the last 160 bytes. */
    return (char *)(si.buf + si.size - ARGV_SAFE_SIZE);
}

static void run_line(char *line)
{
    char *argv[16]; int argc = 0;
    /* Single shared expansion buffer; expanded tokens written back-to-back
       and pointers stored in argv. */
    static char expanded[CCP_LINE_MAX * 4];
    int eoff = 0;
    char *p = line;
    while (argc < 16) {
        char *t = next_token(&p);
        if (!t) break;
        if (eoff + CCP_LINE_MAX > (int)sizeof(expanded)) break;
        /* First do $VAR/%VAR% expansion. */
        char tmp[CCP_LINE_MAX];
        expand_token(t, tmp, CCP_LINE_MAX);
        /* For non-builtin contexts, expand wildcards.  We do it for argv[1..]
           only -- argv[0] (command) is taken literally. */
        if (argc > 0 && has_wildcard(tmp)) {
            int got = glob_expand(tmp, &expanded[eoff],
                                  (int)sizeof(expanded) - eoff,
                                  &argv[argc], 16 - argc);
            if (got == 0) {
                /* No matches: pass the literal pattern through. */
                int n = 0;
                while (tmp[n] && eoff + n < (int)sizeof(expanded) - 1)
                    { expanded[eoff + n] = tmp[n]; n++; }
                expanded[eoff + n] = 0;
                argv[argc] = &expanded[eoff];
                eoff += n + 1;
                argc++;
            } else {
                argc += got;
                /* glob_expand wrote into expanded[eoff..] -- advance eoff
                   past the last written entry. */
                if (got > 0) {
                    char *last = argv[argc - 1];
                    eoff = (int)(last - expanded) + (int)strlen_(last) + 1;
                }
            }
            continue;
        }
        /* No wildcard: just copy. */
        int n = 0;
        while (tmp[n] && eoff + n < (int)sizeof(expanded) - 1)
            { expanded[eoff + n] = tmp[n]; n++; }
        expanded[eoff + n] = 0;
        argv[argc] = &expanded[eoff];
        eoff += n + 1;
        argc++;
    }
    if (argc == 0) return;
    if (try_drive_switch(argv[0])) return;

    /* Strip redirection tokens from argv before dispatch: ">", ">>", "<".
       For each match, the next token is the filename.  Sets up the redirect
       via BDOS 114; calls BDOS 115 after the command finishes. */
    const char *redir_out = 0;
    const char *redir_in  = 0;
    int          redir_append = 0;
    {
        char *new_argv[16];
        int new_argc = 0;
        for (int i = 0; i < argc; i++) {
            if ((argv[i][0] == '>' && argv[i][1] == 0) ||
                (argv[i][0] == '>' && argv[i][1] == '>' && argv[i][2] == 0)) {
                int append = (argv[i][1] == '>');
                if (i + 1 < argc) {
                    redir_out = argv[i + 1];
                    redir_append = append;
                    i++;
                    continue;
                }
            }
            if (argv[i][0] == '<' && argv[i][1] == 0) {
                if (i + 1 < argc) { redir_in = argv[i + 1]; i++; continue; }
            }
            new_argv[new_argc++] = argv[i];
        }
        for (int i = 0; i < new_argc; i++) argv[i] = new_argv[i];
        argc = new_argc;
    }

    /* The bdos(116)/(114) traps walk FAT, which uses ~1.1 KB of
       kernel stack and overflows into the top of CCP BSS, sometimes
       stomping the `expanded[]` buffer where argv strings live.
       Before the trap, copy each arg into the static scratch below
       so the post-trap dispatch can rely on them.
       Static (BSS, low address) — putting this on stack would just
       push the kernel's overflow line that much deeper. */
    if (redir_in || redir_out) {
        char *argv_safe = argv_safe_ptr();
        int off = 0;
        for (int i = 0; i < argc && off < argv_safe_room - 1; i++) {
            const char *p = argv[i];
            char *dst = &argv_safe[off];
            int n = 0;
            while (*p && off + n < argv_safe_room - 1) { dst[n++] = *p++; }
            dst[n] = 0;
            argv[i] = dst;
            off += n + 1;
        }
    }

    int redir_open = 0, inrd_open = 0;
    if (redir_out && bdos(114, (u32)redir_out, (u32)redir_append) == 0)
        redir_open = 1;
    if (redir_in  && bdos(116, (u32)redir_in,  0) == 0)
        inrd_open = 1;

    /* Tri-vocabulary: rewrite argv[0] through the alias table before lookup. */
    const char *canon = resolve_alias(argv[0]);
    for (const struct builtin *b = builtins; b->name; b++) {
        if (strieq(canon, b->name)) {
            b->fn(argc, argv);
            if (redir_open) bdos(115, 0, 0);
            if (inrd_open)  bdos(117, 0, 0);
            return;
        }
    }

    /* Auto-load: try to find argv[0] as a file on the FAT volume. */
    {
        /* Reassemble argv[1..] into the command tail. */
        static char cmdtail[CCP_LINE_MAX];
        int tn = 0;
        for (int a = 1; a < argc && tn < (int)sizeof(cmdtail) - 1; a++) {
            if (a > 1) cmdtail[tn++] = ' ';
            const char *p = argv[a];
            while (*p && tn < (int)sizeof(cmdtail) - 1) cmdtail[tn++] = *p++;
        }
        cmdtail[tn] = 0;
        u32 rc = bdos(113, (u32)argv[0], (u32)cmdtail);
        if (rc == 0) {
            /* Auto-load returned (program ran).  Close redirect if any.
               (loadrun already warm-boots if it actually launched a program;
               this path runs only if the load failed before launch.) */
            if (redir_open) bdos(115, 0, 0);
            if (inrd_open)  bdos(117, 0, 0);
            return;
        }
    }

    if (needs_fs(argv[0])) {
        puts_("?"); puts_(argv[0]);
        puts_(": filesystem not available yet (FAT32 layer pending)\r\n");
        if (redir_open) bdos(115, 0, 0);
        if (inrd_open)  bdos(117, 0, 0);
        return;
    }
    puts_("?unknown command: "); puts_(argv[0]);
    puts_("\r\nType `help` for a list.\r\n");
    if (redir_open) bdos(115, 0, 0);
    if (inrd_open)  bdos(117, 0, 0);
}

/*--------------------------------------------------------------------------
 * cmd_submit -- run lines from a file as commands.
 *
 * Reads <file> via BDOS 121 (load-file-to-buffer), splits at \n,
 * trims \r and # comments, and feeds each non-empty line to run_line.
 * Lines are echoed prefixed with "> " so the user can see what's
 * executing.  Lines starting with @ are silent (DOS .BAT convention).
 *--------------------------------------------------------------------------*/
/* Script-driven input mode.  All state lives in the kernel-side
   lzss_window scratch buffer (BDOS 122) so deep kernel traps during
   nested run_line() invocations can't corrupt it.  Layout:
       offset  0..3   active (u32, 0 or 1)
       offset  4..7   pos    (u32, byte index into script)
       offset  8..11  len    (u32, script length in bytes)
       offset 16+     script bytes
*/
struct submit_hdr { u32 active; u32 pos; u32 len; u32 _pad; };
#define SUBMIT_DATA_OFFSET 16

/* Cache of the kernel scratch pointer; reset on each cmd_submit. */
static struct submit_hdr *submit_state(void)
{
    struct { u8 *buf; u32 size; } si;
    bdos(122, (u32)&si, 0);
    return (struct submit_hdr *)si.buf;
}

/* LOAD — read a FAT file into memory at addr.  Returns bytes read.
 *   load <addr> <file> [maxlen]
 * Without maxlen, reads up to 0x8000 bytes (sane default).  Pairs
 * with SAVE for snapshot/restore workflows.
 */
static int cmd_load(int argc, char **argv) {
    if (argc < 3) { usage_(argv, "<addr> <file> [maxlen]"); return 1; }
    if (!ensure_mounted_()) return 1;
    u32 addr, max = 0x8000;
    if (!parse_num(argv[1], &addr)) { errf_(argv[0], argv[1], "bad addr"); return 1; }
    if (argc >= 4 && !parse_num(argv[3], &max)) {
        errf_(argv[0], argv[3], "bad maxlen"); return 1;
    }
    struct { u8 *buf; u32 max; } lb = { (u8 *)addr, max };
    u32 r = bdos(121, (u32)argv[2], (u32)&lb);
    if (r == 0xFFFFFFFFu) { errf_(argv[0], argv[2], "load failed"); return 1; }
    puts_("ok ("); putdec(r); puts_(" bytes at 0x"); puthex(addr, 6); puts_(")\r\n");
    return 0;
}

/* SAVE — dump a memory range to a FAT file.
 *   save <addr> <len> <file>
 * Both addr and len accept decimal or 0xHEX.  Useful for snapshotting
 * loaded program images or arbitrary memory after `peek`/`poke` work.
 */
static int cmd_save(int argc, char **argv) {
    if (argc < 4) { usage_(argv, "<addr> <len> <file>"); return 1; }
    if (!ensure_mounted_()) return 1;
    u32 addr, len;
    if (!parse_num(argv[1], &addr)) { errf_(argv[0], argv[1], "bad addr"); return 1; }
    if (!parse_num(argv[2], &len))  { errf_(argv[0], argv[2], "bad len");  return 1; }
    struct { const u8 *buf; u32 size; } sb = { (const u8 *)addr, len };
    if (bdos(120, (u32)argv[3], (u32)&sb) != 0) {
        errf_(argv[0], argv[3], "save failed"); return 1;
    }
    puts_("ok ("); putdec(len); puts_(" bytes)\r\n");
    return 0;
}

static int cmd_submit(int argc, char **argv) {
    if (argc < 2) { usage_(argv, "<script>"); return 1; }
    if (!ensure_mounted_()) return 1;
    struct { u8 *buf; u32 size; } si;
    bdos(122, (u32)&si, 0);
    u32 max_script = si.size - SUBMIT_DATA_OFFSET;
    struct { u8 *buf; u32 max; } lb = { si.buf + SUBMIT_DATA_OFFSET, max_script };
    u32 n = bdos(121, (u32)argv[1], (u32)&lb);
    if (n == 0xFFFFFFFFu) { errf_("submit", argv[1], "not found"); return 1; }
    if (n >= max_script)  { errf_("submit", argv[1], "too big");   return 1; }
    struct submit_hdr *h = (struct submit_hdr *)si.buf;
    h->len    = n;
    h->pos    = 0;
    h->active = 1;
    return 0;
}

/* Test whether a script is currently driving input. */
static int submit_is_active(void)
{
    return submit_state()->active != 0;
}

/* Pull the next script line into `out` (max CCP_LINE_MAX bytes).
   Returns line length, or -1 on exhaustion (clears active). */
static int submit_next_line(char *out, int *silent_out)
{
    *silent_out = 0;
    struct submit_hdr *h = submit_state();
    if (!h->active) return -1;
    u8 *script = (u8 *)h + SUBMIT_DATA_OFFSET;
    int ln = 0;
    while (h->pos < h->len) {
        u8 c = script[h->pos++];
        if (c == 0x1A || c == 0) break;
        if (c == '\r') continue;
        if (c == '\n') {
            out[ln] = 0;
            if (ln == 0 || out[0] == '#') { ln = 0; continue; }
            if (out[0] == '@') {
                *silent_out = 1;
                for (int i = 0; i < ln; i++) out[i] = out[i + 1];
                ln--;
            }
            return ln;
        }
        if (ln < CCP_LINE_MAX) out[ln++] = (char)c;
    }
    h->active = 0;
    if (ln > 0 && out[0] != '#') {
        out[ln] = 0;
        if (out[0] == '@') {
            *silent_out = 1;
            for (int i = 0; i < ln; i++) out[i] = out[i + 1];
            ln--;
        }
        return ln;
    }
    return -1;
}

/*--------------------------------------------------------------------------
 * Prompt
 *--------------------------------------------------------------------------*/
/*--------------------------------------------------------------------------
 * Prompt: expand $X tokens from PROMPT env var (DOS-style).
 *   $P  current path (drive + user)
 *   $G  '>'
 *   $L  '<'
 *   $N  current drive letter only
 *   $$  literal '$'
 *   $S  space
 *   $_  CR LF
 *   $E  ESC
 * Default if unset: "$P$G "
 *--------------------------------------------------------------------------*/
static int build_prompt(char *out)
{
    const char *fmt = env_get("PROMPT");
    if (!fmt) fmt = "$P$G$S";        /* default */

    u32 d = bdos(25, 0, 0);
    u32 u = bdos(32, 0xFF, 0) & 0xFF;
    static char cwd[40];
    bdos(125, (u32)cwd, sizeof(cwd));
    int n = 0;
    while (*fmt && n < 30) {
        if (*fmt != '$') { out[n++] = *fmt++; continue; }
        fmt++;
        char tok = *fmt++;
        switch (tok) {
        case 'P':
            out[n++] = (char)('A' + (int)(d & 0x0F));
            if (u > 0) {
                if (u >= 10) out[n++] = '0' + (u / 10);
                out[n++] = '0' + (u % 10);
            }
            out[n++] = ':';
            for (int i = 0; cwd[i] && n < 30; i++) out[n++] = cwd[i];
            out[n++] = '\\';
            break;
        case 'N': out[n++] = (char)('A' + (int)(d & 0x0F)); break;
        case 'G': out[n++] = '>'; break;
        case 'L': out[n++] = '<'; break;
        case 'S': out[n++] = ' '; break;
        case '$': out[n++] = '$'; break;
        case 'E': out[n++] = 0x1B; break;
        case '_': out[n++] = '\r'; out[n++] = '\n'; break;
        case 0:   fmt--; break;     /* trailing $ */
        default:  out[n++] = tok; break;
        }
    }
    out[n] = 0;
    return n;
}

/* If `line` contains a top-level " | " separator (outside any quoted
   region), transform it into two sequential run_line invocations with
   a temp file standing in for the pipe.  Returns 1 if a pipe was
   handled (caller should not re-dispatch), 0 if there was no pipe. */
static int handle_pipe(const char *line)
{
    /* Locate the first unquoted " | ". */
    const char *p = line;
    int in_squote = 0, in_dquote = 0;
    const char *bar = 0;
    while (*p) {
        if (*p == '\'' && !in_dquote) in_squote = !in_squote;
        else if (*p == '"' && !in_squote) in_dquote = !in_dquote;
        else if (*p == '|' && !in_squote && !in_dquote &&
                 p > line && p[-1] == ' ' && p[1] == ' ') { bar = p; break; }
        p++;
    }
    if (!bar) return 0;

    static const char *const TMPNAME = "PIPE.TMP";
    /* Pipe scratch lives in the kernel-side lzss_window at a region
       that's been quiet — past the inrd buffer, before argv_safe.
       Putting these in CCP BSS (where they used to live) caused them
       to be stomped by the kernel-stack overflow during the first
       run_line's bdos(114) trap. */
    struct { u8 *buf; u32 size; } si;
    bdos(122, (u32)&si, 0);
    char *left  = (char *)(si.buf + 3616);     /* 160 bytes */
    char *right = (char *)(si.buf + 3776);     /* 160 bytes */
    int li = 0, ri = 0;
    const int LMAX = 160, RMAX = 160;

    /* Left half + " > PIPE.TMP" */
    int llen = (int)(bar - line);
    while (llen > 0 && line[llen - 1] == ' ') llen--;
    for (int i = 0; i < llen && li < LMAX - 16; i++) left[li++] = line[i];
    const char *suf = " > "; while (*suf) left[li++] = *suf++;
    const char *t = TMPNAME; while (*t) left[li++] = *t++;
    left[li] = 0;

    /* Right half + " < PIPE.TMP" */
    p = bar + 1;
    while (*p == ' ') p++;
    while (*p && ri < RMAX - 16) right[ri++] = *p++;
    suf = " < "; while (*suf) right[ri++] = *suf++;
    t = TMPNAME; while (*t) right[ri++] = *t++;
    right[ri] = 0;

    run_line(left);
    run_line(right);

    /* Clean up the temp file. */
    u8 fcb[36];
    build_fcb(fcb, TMPNAME);
    bdos(19, (u32)fcb, 0);
    return 1;
}

void _start(void)
{
    puts_("\r\n[CCP loaded into TPA from compressed ROM image]\r\n");
    static char line[CCP_LINE_MAX];
    static char prompt[16];

    for (;;) {
        int plen = build_prompt(prompt);
        line[0] = 0;
        int n;
        if (submit_is_active()) {
            int silent = 0;
            n = submit_next_line(line, &silent);
            if (n < 0) continue;
            if (!silent) { puts_(prompt); puts_(line); puts_("\r\n"); }
        } else {
            n = read_line(prompt, plen, line);
            line[n] = 0;
            if (n > 0 && line[0] == '!') {
                int rc = hist_substitute(line);
                if (rc < 0) continue;
                if (rc > 0) n = strlen_(line);
            }
            if (n > 0 && line[0] != ' ') history_add(line);
        }
        if (handle_pipe(line)) continue;
        run_line(line);
    }
}
