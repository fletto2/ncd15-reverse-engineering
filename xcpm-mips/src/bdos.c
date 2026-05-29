/*============================================================================
 * bdos.c -- XCP/M-68K BDOS skeleton (§8)
 *
 * Implements the floor set of functions that the CCP and console-bound
 * programs require: 0, 1, 2, 6, 9, 10, 11, 12, 25, 32, 50, 62.
 * Disk-related functions (13-22, 27-48 etc.) are stubs returning safe
 * defaults; FAT32-backed file I/O is wired in a later slice.
 *==========================================================================*/
#include "xcpm.h"
#include "bdos.h"
#include "bios.h"

/* Mutable kernel state in BSS */
static u8   cur_drive;        /* default drive (0 = A:) */
static u8   cur_user;         /* user area 0..15 */
static char cur_subpath[40];  /* "" = root; "/SUB1/SUB2" form, no trailing / */

/*-- Small helpers ---------------------------------------------------------*/
static void put_str(const char *p)
{
    /* NB: BDOS 9 is $-terminated, not null-terminated. We re-export
       a null-terminated helper for kernel use. */
    while (*p) bios_call(BIOS_CONOUT, (u32)*p++, 0);
}

/*-- 0: System Reset (warm boot) ------------------------------------------*/
extern void warmboot(void);     /* in main.c */
static u32 d_reset(u32 a, u32 b)        { (void)a;(void)b;
    /* Don't return to the loaded program -- jump straight to warmboot.
       This abandons the program's stack frame; XCP/M is single-mode
       and warmboot will reset SP=top-of-RAM. */
#if defined(__mips__)
    __asm__ volatile (
        "la   $sp, _stack_top\n\t"      /* reset stack to top of region */
        "j    warmboot\n\t"
        "nop\n\t"
        : : : "memory"
    );
#else
    __asm__ volatile (
        "move.l #_stack_top, %%a7\n\t"  /* reset SSP to top of RAM */
        "move.w #0x2700, %%sr\n\t"      /* mask interrupts */
        "jmp warmboot\n\t"
        : : : "memory"
    );
#endif
    /* unreachable */
    return 0;
}

/*-- 1: Console In (echoed) ------------------------------------------------*/
static u32 d_conin(u32 a, u32 b)        { (void)a;(void)b;
    u32 c = bios_call(BIOS_CONIN, 0, 0);
    if (c >= ' ' || c == '\r' || c == '\n' || c == '\t' || c == 0x08)
        bios_call(BIOS_CONOUT, c, 0);
    return c;
}

/*-- 2: Console Out --------------------------------------------------------*/
static u32 d_conout(u32 c, u32 b)       { (void)b;
    bios_call(BIOS_CONOUT, c & 0xFF, 0);
    return 0;
}

/*-- 6: Direct Console I/O -------------------------------------------------*/
static u32 d_directio(u32 c, u32 b)     { (void)b;
    if ((c & 0xFF) == 0xFF) {
        /* Read; nonblocking */
        int r = uart_poll();
        return (r < 0) ? 0 : (u32)(r & 0xFF);
    } else if ((c & 0xFF) == 0xFE) {
        return (uart_poll() >= 0) ? 0xFF : 0;
    } else {
        bios_call(BIOS_CONOUT, c & 0xFF, 0);
        return 0;
    }
}

/*-- 9: Print String ($-terminated) ----------------------------------------*/
static u32 d_printstr(u32 ptr, u32 b)   { (void)b;
    const char *p = (const char *)ptr;
    while (*p && *p != '$')
        bios_call(BIOS_CONOUT, (u32)*p++, 0);
    return 0;
}

/*-- 10: Read Console Buffer (line edit) -----------------------------------*/
struct linebuf { u8 max; u8 len; u8 data[]; };

static u32 d_readline(u32 ptr, u32 b)   { (void)b;
    struct linebuf *lb = (struct linebuf *)ptr;
    u8 max = lb->max, n = 0;

    for (;;) {
        u32 c = bios_call(BIOS_CONIN, 0, 0) & 0xFF;
        if (c == '\r' || c == '\n') {
            bios_call(BIOS_CONOUT, '\r', 0);
            bios_call(BIOS_CONOUT, '\n', 0);
            break;
        }
        if (c == 0x08 || c == 0x7F) {                /* BS / DEL */
            if (n > 0) {
                n--;
                bios_call(BIOS_CONOUT, '\b', 0);
                bios_call(BIOS_CONOUT, ' ',  0);
                bios_call(BIOS_CONOUT, '\b', 0);
            }
            continue;
        }
        if (c == 0x15) {                              /* ^U: kill line */
            while (n > 0) {
                bios_call(BIOS_CONOUT, '\b', 0);
                bios_call(BIOS_CONOUT, ' ',  0);
                bios_call(BIOS_CONOUT, '\b', 0);
                n--;
            }
            continue;
        }
        if (c == 0x03) {                              /* ^C: warm boot */
            return d_reset(0, 0);
        }
        if (n < max) {
            lb->data[n++] = (u8)c;
            bios_call(BIOS_CONOUT, c, 0);
        }
    }
    lb->len = n;
    return 0;
}

/*-- 11: Console Status ----------------------------------------------------*/
static u32 d_constat(u32 a, u32 b)      { (void)a;(void)b;
    return bios_call(BIOS_CONSTAT, 0, 0);
}

/*-- 12: Get Version (returns 0x0022 = CP/M-68K v2.2 compat) ---------------*/
static u32 d_version(u32 a, u32 b)      { (void)a;(void)b;
    return 0x0022;
}

/*-- 25: Get Current Disk --------------------------------------------------*/
static u32 d_getdisk(u32 a, u32 b)      { (void)a;(void)b;
    return cur_drive;
}

/*-- 14: Select Disk -------------------------------------------------------*/
static u32 d_seldisk(u32 d, u32 b)      { (void)b;
    cur_drive = (u8)(d & 0x0F);
    cur_subpath[0] = 0;     /* drive change resets cwd */
    return 0;
}

/*-- 32: Get / Set User ----------------------------------------------------*/
static u32 d_user(u32 u, u32 b)         { (void)b;
    if ((u & 0xFF) == 0xFF) return cur_user;
    cur_user = (u8)(u & 0x0F);
    return 0;
}

/*-- 50: Direct BIOS Call (DDT uses this) ---------------------------------*/
static u32 d_directbios(u32 ptr, u32 b) { (void)b;
    /* Convention: ptr -> struct { u32 fn; u32 d1; u32 d2; } */
    struct { u32 fn; u32 d1; u32 d2; } *bp = (void *)ptr;
    if (!bp || bp->fn >= BIOS_NFUNCS) return 0;
    return bios_dispatch(bp->fn, bp->d1, bp->d2);
}

/*-- 62: Set Exception Vector (wraps BIOS 22) ------------------------------*/
static u32 d_setexc(u32 vec, u32 addr)  {
    return bios_dispatch(22, vec, addr);
}

/* Forward decls for path-mapping helpers used by ext functions below. */
static int build_path(u8 drive, u8 user, const char *name, char *out, int max);

/*-- XCP/M-68K extension functions ----------------------------------------
 *
 * 110: list root directory of mounted FAT volume.  parm1 = pointer to
 *      a callback function with signature (void)(const char *name,
 *      const char *lfn, u32 size, u8 attr).  parm2 = caller's ctx.
 *      For phase-1 simplicity we instead just print to console here.
 *
 * 111: read file `name` (parm1) into buffer `parm2` (struct {addr, max}).
 *      Returns bytes read or -1.
 *
 * 112: mount/probe the FAT layer; returns FAT type (12/16/32) or 0.
 *--------------------------------------------------------------------------*/
#include "fat.h"

static u32 d_ext_mount(u32 a, u32 b)    { (void)a;(void)b;
    int rc = fat_mount();
    if (rc != 0) return 0;
    const fat_mount_t *m = fat_get_mount();
    return (u32)m->type;
}

/* parm1 = NULL: prints to console; otherwise unsupported callback hook. */
static u32 d_ext_listroot(u32 cb, u32 ctx) { (void)cb; (void)ctx;
    char dir_path[40];
    build_path(cur_drive, cur_user, 0, dir_path, sizeof(dir_path));
    fat_dir_iter_t it;
    if (fat_diropen_path(&it, dir_path) != 0) {
        return 0xFFFFFFFFu;
    }
    fat_dirent_t de;
    char lfn[256];
    u32 count = 0;
    while (fat_dirnext(&it, &de, lfn, sizeof(lfn)) == 0) {
        u32 char_count = 0;
        /* "name.ext" 8.3 */
        for (int i = 0; i < 8 && de.name[i] != ' '; i++) {
            bios_call(BIOS_CONOUT, de.name[i], 0); char_count++;
        }
        if (de.name[8] != ' ') {
            bios_call(BIOS_CONOUT, '.', 0); char_count++;
            for (int i = 0; i < 3 && de.name[8+i] != ' '; i++) {
                bios_call(BIOS_CONOUT, de.name[8+i], 0); char_count++;
            }
        }
        while (char_count < 14) { bios_call(BIOS_CONOUT, ' ', 0); char_count++; }
        bios_call(BIOS_CONOUT, ' ', 0);
        if (de.attr & FAT_ATTR_DIR) {
            bios_call(BIOS_CONOUT, '<', 0);
            bios_call(BIOS_CONOUT, 'D', 0);
            bios_call(BIOS_CONOUT, 'I', 0);
            bios_call(BIOS_CONOUT, 'R', 0);
            bios_call(BIOS_CONOUT, '>', 0);
        } else {
            char buf[12]; int i = 11; buf[i--] = 0;
            u32 v = de.file_size;
            if (v == 0) buf[i--] = '0';
            else { while (v && i >= 0) { buf[i--] = '0' + (v % 10); v /= 10; } }
            const char *p = &buf[i + 1];
            while (*p) bios_call(BIOS_CONOUT, *p++, 0);
        }
        if (lfn[0]) {
            bios_call(BIOS_CONOUT, ' ', 0); bios_call(BIOS_CONOUT, ' ', 0);
            const char *p = lfn;
            while (*p) bios_call(BIOS_CONOUT, *p++, 0);
        }
        bios_call(BIOS_CONOUT, '\r', 0);
        bios_call(BIOS_CONOUT, '\n', 0);
        count++;
    }
    return count;
}

static int ensure_mounted(void);    /* forward */
extern const char *const mount_paths[16];   /* fwd to defn lower in file */

/*============================================================================
 * Output redirection support (BDOS 114/115).
 *
 *   bdos(114, name, mode)  -- open a redirect: 0=truncate (>), 1=append (>>)
 *   bdos(115, 0, 0)        -- flush + close the active redirect
 *
 * While a redirect is active, BIOS conout (called by BDOS 2/9 and most
 * other text-output paths) buffers bytes to RAM and flushes them to the
 * file in 128-byte chunks via fat_fwrite.  This keeps per-byte CCP-style
 * output fast even with bit-banged SD writes.
 *==========================================================================*/
static fat_file_t  redir_file;
static u8          redir_active;
static u8          redir_buf[128];
static u8          redir_buf_n;

static void redir_flush(void)
{
    if (!redir_active || redir_buf_n == 0) return;
    fat_fwrite(&redir_file, redir_buf, redir_buf_n);
    redir_buf_n = 0;
}

/* Called by BIOS conout to consume one redirected byte; returns 1 if the
   byte was consumed, 0 if no redirect is active (caller must send to UART). */
int bdos_redir_putc(int c)
{
    if (!redir_active) return 0;
    redir_buf[redir_buf_n++] = (u8)(c & 0xFF);
    if (redir_buf_n >= sizeof(redir_buf)) redir_flush();
    return 1;
}

/* BDOS 114: begin redirect.  parm1 = filename (NUL-terminated string).
   parm2 bit 0 = append mode (1) or truncate (0).
   Returns 0 on success, 0xFF on failure. */
static u32 d_ext_redir_begin(u32 name_p, u32 mode)
{
    if (ensure_mounted() != 0) return 0xFF;
    if (redir_active) redir_flush();        /* flush previous, just in case */

    char path[40];
    build_path(cur_drive, cur_user, (const char *)name_p, path, sizeof(path));

    int append = (mode & 1);
    int rc;
    if (append) {
        rc = fat_fopen(path, &redir_file);
        if (rc != 0) {
            rc = fat_fcreate(path, &redir_file);
            if (rc != 0) return 0xFF;
        } else {
            fat_fseek(&redir_file, redir_file.file_size);
        }
    } else {
        rc = fat_fcreate(path, &redir_file);
        if (rc != 0) return 0xFF;
    }
    redir_buf_n = 0;
    redir_active = 1;
    return 0;
}

/* BDOS 115: end redirect.  Flushes buffered bytes and closes the file. */
static u32 d_ext_redir_end(u32 a, u32 b) { (void)a;(void)b;
    if (!redir_active) return 0;
    redir_flush();
    fat_fflush(&redir_file);
    fat_fclose(&redir_file);
    redir_active = 0;
    redir_buf_n = 0;
    return 0;
}

/* Input redirection (BDOS 116/117).
 *
 * State and buffer live in the upper half of the kernel's lzss_window
 * scratch (see BDOS 122 for the full layout).  Doing it this way costs
 * zero kernel BSS — important because PVS-2 has only ~32 bytes of
 * BSS-vs-TPA headroom.
 *
 *   Inrd area (offset 2048..4095 in lzss_window):
 *      + 0  u32 active (0|1)
 *      + 4  u32 pos
 *      + 8  u32 len
 *      +12  u32 _pad
 *      +16  N  buffer (up to 2032 bytes of file contents)
 */
#define INRD_AREA_OFF  2048
#define INRD_AREA_SIZE 2048
#define INRD_DATA_OFF  16

struct inrd_hdr { u32 active; u32 pos; u32 len; u32 _pad; };

static struct inrd_hdr *inrd_state(void)
{
    return (struct inrd_hdr *)(lzss_scratch_buf() + INRD_AREA_OFF);
}

int bdos_redir_getc(void)
{
    struct inrd_hdr *h = inrd_state();
    if (!h->active) return -1;
    if (h->pos >= h->len) return 0x1A;            /* CP/M EOF marker */
    return ((u8 *)h)[INRD_DATA_OFF + h->pos++];
}

int bdos_redir_status(void)
{
    struct inrd_hdr *h = inrd_state();
    if (!h->active) return -1;
    return 0xFF;                                   /* always ready */
}

static u32 d_ext_inrd_begin(u32 name_p, u32 b)
{
    (void)b;
    if (ensure_mounted() != 0) return 0xFF;
    struct inrd_hdr *h = inrd_state();
    /* Read file into the buffer immediately following the header. */
    u32 buf_max = INRD_AREA_SIZE - INRD_DATA_OFF;
    fat_file_t fp;
    char path[40];
    build_path(cur_drive, cur_user, (const char *)name_p, path, sizeof(path));
    if (fat_fopen(path, &fp) != 0) return 0xFF;
    int n = fat_fread(&fp, ((u8 *)h) + INRD_DATA_OFF, buf_max);
    fat_fclose(&fp);
    if (n < 0) return 0xFF;
    h->len    = (u32)n;
    h->pos    = 0;
    h->active = 1;
    return 0;
}

static u32 d_ext_inrd_end(u32 a, u32 b)
{
    (void)a; (void)b;
    inrd_state()->active = 0;
    return 0;
}

/* BDOS 123: byte-precise file copy.  CP/M's FCB-based 20/21 records
 * round to 128 bytes; using fat_fopen/fcreate/fread/fwrite directly
 * preserves the source's exact byte length on the destination.
 *   parm1 = src path, parm2 = dst path (both NUL-terminated)
 * Returns bytes copied, or 0xFFFFFFFFu on error.
 */
static u32 d_ext_copy(u32 src_p, u32 dst_p)
{
    if (ensure_mounted() != 0) return 0xFFFFFFFFu;
    char src_full[40], dst_full[40];
    build_path(cur_drive, cur_user, (const char *)src_p, src_full, sizeof(src_full));
    build_path(cur_drive, cur_user, (const char *)dst_p, dst_full, sizeof(dst_full));

    fat_file_t sf;
    if (fat_fopen(src_full, &sf) != 0) return 0xFFFFFFFFu;
    /* Snapshot size up front so we don't depend on fat_fsize after
       sf might be invalidated. */
    u32 size = sf.file_size;

    fat_file_t df;
    if (fat_fcreate(dst_full, &df) != 0) {
        fat_fclose(&sf);
        return 0xFFFFFFFFu;
    }

    /* Stream through a 128-byte chunk on stack — avoids needing a
       big kernel buffer.  Kernel stack pressure is comparable to a
       single fat_fopen call (already known-good for mkdir et al). */
    u8 chunk[128];
    u32 copied = 0;
    while (copied < size) {
        int n = fat_fread(&sf, chunk, sizeof(chunk));
        if (n <= 0) break;
        if (fat_fwrite(&df, chunk, n) != n) { copied = 0xFFFFFFFFu; break; }
        copied += (u32)n;
    }
    fat_fflush(&df);
    fat_fclose(&df);
    fat_fclose(&sf);
    return copied;
}

/* BDOS 122: get pointer to the kernel's 4 KB LZSS-window buffer for
 * use as CCP scratch.  Idle between warmboots; clobbered when the user
 * triggers a reboot.  parm1 = pointer to struct { u8 *buf; u32 size }
 * filled in by the call.  Returns 0.
 */
struct scratch_info { u8 *buf; u32 size; };

static u32 d_ext_scratch(u32 si_p, u32 b)
{
    (void)b;
    struct scratch_info *si = (struct scratch_info *)si_p;
    if (!si) return 0xFF;
    si->buf  = lzss_scratch_buf();
    si->size = lzss_scratch_size();
    return 0;
}

/* BDOS 120: save buffer to file.
 *   parm1 = path (NUL-terminated)
 *   parm2 = pointer to struct { const u8 *buf; u32 size; }
 * Truncates/creates the destination, writes `size` bytes from `buf`.
 * Returns 0 on success, 0xFFFFFFFFu on error.
 */
struct savebuf { const u8 *buf; u32 size; };

static u32 d_ext_savefile(u32 path_p, u32 sb_p)
{
    if (ensure_mounted() != 0) return 0xFFFFFFFFu;
    struct savebuf *sb = (struct savebuf *)sb_p;
    if (!sb || !sb->buf) return 0xFFFFFFFFu;

    char path[40];
    build_path(cur_drive, cur_user, (const char *)path_p, path, sizeof(path));
    fat_file_t fp;
    if (fat_fcreate(path, &fp) != 0) return 0xFFFFFFFFu;
    int wrote = fat_fwrite(&fp, sb->buf, sb->size);
    fat_fflush(&fp);
    fat_fclose(&fp);
    return ((u32)wrote == sb->size) ? 0 : 0xFFFFFFFFu;
}

/* BDOS 121: load file into caller's buffer.
 *   parm1 = path (NUL-terminated)
 *   parm2 = pointer to struct { u8 *buf; u32 max; }
 * Returns bytes read on success, 0xFFFFFFFFu on error (file not found).
 */
struct loadbuf { u8 *buf; u32 max; };

static u32 d_ext_loadfile(u32 path_p, u32 lb_p)
{
    if (ensure_mounted() != 0) return 0xFFFFFFFFu;
    struct loadbuf *lb = (struct loadbuf *)lb_p;
    if (!lb || !lb->buf || lb->max == 0) return 0xFFFFFFFFu;

    fat_file_t fp;
    char path[40];
    build_path(cur_drive, cur_user, (const char *)path_p, path, sizeof(path));
    if (fat_fopen(path, &fp) != 0) return 0xFFFFFFFFu;

    u32 total = 0;
    while (total < lb->max) {
        int n = fat_fread(&fp, lb->buf + total, lb->max - total);
        if (n <= 0) break;
        total += (u32)n;
    }
    fat_fclose(&fp);
    return total;
}

/* BDOS 127: stream-decompress an LZSS blob to the console.  Lets the
 * CCP store help text (and any other long static text) compressed in
 * its .rodata, expanding it on demand without needing a big output
 * buffer (only the kernel's 4 KB sliding window, which lives in BSS
 * anyway). */
static void emit_to_conout(u8 c) { bios_call(BIOS_CONOUT, c, 0); }

static u32 d_ext_lzss_print(u32 blob_p, u32 blob_size)
{
    return lzss_decompress_stream((const u8 *)blob_p, blob_size, emit_to_conout);
}

/* BDOS 124: chdir.  Treats parm1 as a NUL-terminated path:
 *   "/"     -> reset to root
 *   ".."    -> pop the last component of cur_subpath
 *   "FOO"   -> append "/FOO" (must be a directory)
 *   "/A/B"  -> set absolute "/A/B" (must walk to a directory)
 *
 * Returns 0 on success, 0xFF on any failure (target not a dir, etc.).
 *
 * Verifies the target by opening it as a directory via fat_diropen_path
 * against a path built from drive + user + new subpath.
 */
static u32 d_ext_chdir(u32 arg_p, u32 b)
{
    (void)b;
    if (ensure_mounted() != 0) return 0xFF;
    const char *arg = (const char *)arg_p;
    if (!arg) return 0xFF;

    /* Compose a candidate new subpath. */
    char ns[40];
    int  nn = 0;
    if (arg[0] == '/') {
        /* Absolute. Skip the leading slash, normalize. */
        const char *p = arg + 1;
        while (*p && nn < (int)sizeof(ns) - 1) {
            if (nn == 0) ns[nn++] = '/';
            ns[nn++] = *p++;
        }
    } else if (arg[0] == '.' && arg[1] == '.' && arg[2] == 0) {
        /* Pop one component from cur_subpath. */
        int len = 0; while (cur_subpath[len]) len++;
        if (len == 0) return 0;       /* already at root */
        int cut = len;
        while (cut > 0 && cur_subpath[cut - 1] != '/') cut--;
        if (cut > 0) cut--;
        for (int i = 0; i < cut && i < (int)sizeof(ns) - 1; i++) ns[nn++] = cur_subpath[i];
    } else if (arg[0] == 0) {
        return 0;                       /* no-op */
    } else {
        /* Relative: append "/arg" to cur_subpath. */
        int len = 0; while (cur_subpath[len] && nn < (int)sizeof(ns) - 1)
            ns[nn++] = cur_subpath[len++];
        if (nn < (int)sizeof(ns) - 1) ns[nn++] = '/';
        const char *p = arg;
        while (*p && nn < (int)sizeof(ns) - 1) ns[nn++] = *p++;
    }
    ns[nn] = 0;

    if (ns[0] == 0) {
        cur_subpath[0] = 0;
        return 0;
    }

    /* Verify by walking to it as a directory. Build a full path:
       <mount[drive]><user-prefix><ns>. We bypass cur_subpath here
       since we're testing the candidate, not the current. */
    char full[80];
    int fn = 0;
    const char *mp = mount_paths[cur_drive];
    while (*mp && fn < (int)sizeof(full) - 1) full[fn++] = *mp++;
    if (cur_user > 0) {
        if (fn + 4 < (int)sizeof(full)) {
            full[fn++] = '/'; full[fn++] = 'U';
            if (cur_user >= 10) full[fn++] = '0' + (cur_user / 10);
            full[fn++] = '0' + (cur_user % 10);
        }
    }
    for (int i = 0; ns[i] && fn < (int)sizeof(full) - 1; i++) full[fn++] = ns[i];
    full[fn] = 0;

    fat_dir_iter_t it;
    if (fat_diropen_path(&it, full) != 0) return 0xFF;

    /* Accepted — commit. */
    int i = 0;
    while (ns[i] && i < (int)sizeof(cur_subpath) - 1) {
        cur_subpath[i] = ns[i]; i++;
    }
    cur_subpath[i] = 0;
    return 0;
}

/* BDOS 125: get cwd subpath as NUL-terminated string into caller's buffer.
   parm1 = buffer pointer, parm2 = buffer size. */
static u32 d_ext_getcwd(u32 buf_p, u32 max)
{
    char *out = (char *)buf_p;
    if (!out || max == 0) return 0xFF;
    u32 i = 0;
    while (cur_subpath[i] && i < max - 1) { out[i] = cur_subpath[i]; i++; }
    out[i] = 0;
    return 0;
}

/* BDOS 118: mkdir <path>; BDOS 119: rmdir <path>.
   Both are root-level only in this phase (no nested paths). */
static u32 d_ext_mkdir(u32 name_p, u32 b)
{
    (void)b;
    if (ensure_mounted() != 0) return 0xFF;
    char path[40];
    build_path(cur_drive, cur_user, (const char *)name_p, path, sizeof(path));
    return (fat_mkdir(path) == 0) ? 0 : 0xFF;
}
static u32 d_ext_rmdir(u32 name_p, u32 b)
{
    (void)b;
    if (ensure_mounted() != 0) return 0xFF;
    char path[40];
    build_path(cur_drive, cur_user, (const char *)name_p, path, sizeof(path));
    int rc = fat_rmdir(path);
    if (rc == 0)  return 0;
    if (rc == -4) return 1;       /* not empty */
    return 0xFF;
}

/*-- 113: ext "load and run a .68K program by name" -----------------------
 * parm1 = char* name (e.g. "STAT" or "stat.68k")
 * parm2 = char* command tail (or 0 for none)
 * Returns 0 on success, 0xFF on file-not-found, error code otherwise.
 *
 * Search policy: try `<name>.68K`, then `<name>.LZ`, on the current drive.
 * For 0x601A binaries: pgmld_run_raw.  For LZSS: pgmld_run_lzss.
 *--------------------------------------------------------------------------*/
extern int pgmld_run_raw (const void *image, u32 size, const char *cmdtail);
extern int pgmld_run_lzss(const void *blob, u32 blob_size, const char *cmdtail);
extern int pgmld_finish_and_run(u32 cseg, u32 size, const char *cmdtail);
/* _tpa_base / _ram_end declared in xcpm.h as u32; reuse those declarations. */

/* Stream a file directly into the TPA -- no kernel buffer.  The CCP
 * currently occupying the TPA gets clobbered, which is fine: warmboot
 * will re-decompress it after the program returns. */
static int try_load_path(const char *path, const char *cmdtail)
{
    /* Resolve relative names through the current drive's mount/user mapping. */
    char full[40];
    build_path(cur_drive, cur_user, path, full, sizeof(full));
    fat_file_t fp;
    if (fat_fopen(full, &fp) != 0) return -1;
    u32 size = fat_fsize(&fp);

    u32 lo = (u32)&_tpa_base;
    u32 cseg = lo + 0x100;
    u8 *dst = (u8 *)cseg;

    /* Read the entire file directly to TPA. */
    u32 got_total = 0;
    while (got_total < size) {
        int got = fat_fread(&fp, dst + got_total, size - got_total);
        if (got <= 0) break;
        got_total += (u32)got;
    }
    fat_fclose(&fp);
    if (got_total != size) return -3;

    /* Detect format from the bytes already in TPA. */
    if (size >= 4 && dst[0] == 'L' && dst[1] == 'Z' &&
        dst[2] == 'S' && dst[3] == 'S')
    {
        /* LZSS-compressed.  We have the compressed image in TPA already.
         * Decompression needs the source somewhere out of the way --
         * copy it up to the top of TPA and decompress from there into
         * cseg.  Only supports compressed-size + uncompressed-size <
         * TPA size. */
        u32 unc_size = ((u32)dst[4] << 24) | ((u32)dst[5] << 16)
                     | ((u32)dst[6] <<  8) |  (u32)dst[7];
        u32 hi = (u32)&_ram_end;
        u32 stack = 0x1000;
        if (unc_size + size + stack > (hi - cseg)) return -2;

        /* Move compressed image to high TPA, then decompress to cseg. */
        u8 *src_buf = (u8 *)(hi - stack - size);
        for (u32 i = size; i > 0; ) { i--; src_buf[i] = dst[i]; }
        /* Now decompress src_buf -> cseg. */
        u32 wrote = lzss_decompress(src_buf, size, dst, hi - cseg);
        if (wrote != unc_size) return -4;
        return pgmld_finish_and_run(cseg, unc_size, cmdtail);
    }

    /* 0x601A or raw — bytes already at cseg. */
    return pgmld_finish_and_run(cseg, size, cmdtail);
}

extern void warmboot(void);

static u32 d_ext_loadrun(u32 name_p, u32 tail_p)
{
    if (ensure_mounted() != 0) return 0xFF;
    const char *name = (const char *)name_p;
    const char *tail = (const char *)tail_p;
    if (tail == 0 || tail_p == 0) tail = "";

    /* Build candidate names: "<name>.68K" and "<name>.LZ" */
    char buf[24]; int n = 0;
    while (*name && n < 16) buf[n++] = *name++;
    int dot = -1;
    for (int i = 0; i < n; i++) if (buf[i] == '.') { dot = i; break; }
    if (dot >= 0) n = dot;

    int found = 0;

    int len = n;
    buf[len++] = '.'; buf[len++] = '6'; buf[len++] = '8'; buf[len++] = 'K';
    buf[len] = 0;
    if (try_load_path(buf, tail) == 0) { found = 1; goto done; }

    len = n;
    buf[len++] = '.'; buf[len++] = 'L'; buf[len++] = 'Z';
    buf[len] = 0;
    if (try_load_path(buf, tail) == 0) { found = 1; goto done; }

done:
    if (!found) return 0xFF;
    /* The program has run and returned.  Close any active redirect so the
       file is flushed before we tear down state and warmboot. */
    if (redir_active) d_ext_redir_end(0, 0);
    d_ext_inrd_end(0, 0);    /* always safe — no-op when not active */
    /* The CCP is gone from TPA.  Reset stack and jump to warmboot to
       re-decompress the CCP and re-enter it.  This call does not return. */
#if defined(__mips__)
    __asm__ volatile (
        "la   $sp, _stack_top\n\t"
        "j    warmboot\n\t"
        "nop\n\t"
        : : : "memory"
    );
#else
    __asm__ volatile (
        "move.l #_stack_top, %%a7\n\t"
        "move.w #0x2700, %%sr\n\t"
        "jmp warmboot\n\t"
        : : : "memory"
    );
#endif
    return 0;
}

struct cat_args { const char *path; u32 max; };

static u32 d_ext_cat(u32 path_p, u32 b)   { (void)b;
    fat_file_t fp;
    const char *user_path = (const char *)path_p;
    char path[40];
    build_path(cur_drive, cur_user, user_path, path, sizeof(path));
    if (fat_fopen(path, &fp) != 0) return 0xFFFFFFFFu;
    u32 total = 0;
    u8 buf[128];
    for (;;) {
        int n = fat_fread(&fp, buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++)
            bios_call(BIOS_CONOUT, buf[i], 0);
        total += (u32)n;
    }
    fat_fclose(&fp);
    return total;
}

/*-- 100: XCP/M-68K identity ----------------------------------------------*/
static u32 d_xcpm_ident(u32 a, u32 b)   { (void)a;(void)b;
    /* Returns magic 'XPMK' = 0x58504D4B */
    return 0x58504D4BUL;
}

/*============================================================================
 * Real BDOS file API on top of the FAT layer.
 *
 * FCB layout reused (36 bytes):
 *   0       drv (0=default, 1=A, ...)
 *   1..11   8.3 name, space-padded
 *   12,13   ex,s1
 *   14      s2
 *   15      rc
 *   16..31  d0..d15  -- private kernel state (we own these)
 *   32      cr
 *   33..35  r0,r1,r2 (random record number)
 *
 * In d0..d15 we stash:
 *   16..19  magic 'XF\0\0' (validates this FCB has been opened by us)
 *   20      handle index (0..FAT_MAX_OPEN-1)
 *   21      generation counter (catches reuse after warm-boot)
 *   22..31  reserved
 *==========================================================================*/

#define FCB_DRV         0
#define FCB_NAME        1
#define FCB_EX          12
#define FCB_S1          13
#define FCB_S2          14
#define FCB_RC          15
#define FCB_PRIV        16
#define FCB_CR          32
#define FCB_R0          33

#define XF_MAGIC0       'X'
#define XF_MAGIC1       'F'

typedef struct {
    u8         in_use;
    u8         generation;
    fat_file_t file;
} fhandle_t;

static fhandle_t fhandles[FAT_MAX_OPEN];
static u8        next_generation = 1;

static u32 cur_dma;     /* BDOS 26 set value */

static int alloc_handle(int *idx_out)
{
    for (int i = 0; i < FAT_MAX_OPEN; i++) {
        if (!fhandles[i].in_use) {
            fhandles[i].in_use = 1;
            fhandles[i].generation = next_generation++;
            if (next_generation == 0) next_generation = 1;
            *idx_out = i;
            return 0;
        }
    }
    return -1;
}

static fhandle_t *resolve_fcb(u8 *fcb)
{
    if (fcb[FCB_PRIV+0] != XF_MAGIC0 || fcb[FCB_PRIV+1] != XF_MAGIC1) return 0;
    int idx = fcb[FCB_PRIV+4];
    u8  gen = fcb[FCB_PRIV+5];
    if (idx < 0 || idx >= FAT_MAX_OPEN) return 0;
    if (!fhandles[idx].in_use)          return 0;
    if (fhandles[idx].generation != gen) return 0;
    return &fhandles[idx];
}

static void stash_handle(u8 *fcb, int idx)
{
    fcb[FCB_PRIV+0] = XF_MAGIC0;
    fcb[FCB_PRIV+1] = XF_MAGIC1;
    fcb[FCB_PRIV+2] = 0;
    fcb[FCB_PRIV+3] = 0;
    fcb[FCB_PRIV+4] = (u8)idx;
    fcb[FCB_PRIV+5] = fhandles[idx].generation;
}

/* Auto-mount the FAT volume if we haven't already. */
static int ensure_mounted(void)
{
    static int mounted = 0;
    if (mounted) return 0;
    int rc = fat_mount();
    if (rc != 0) return -1;
    mounted = 1;
    return 0;
}

/*--------------------------------------------------------------------------
 * Drive-letter and user-area path mapping (§11.1, §11.2).
 *
 *   Drive D + user U + filename FOO ->  <mount[D]>/U<u>/FOO
 *
 * U0 has no /U<n>/ suffix.  Default mount paths: A: -> "" (root),
 * B: -> "/B", C: -> "/C", ...  This keeps the simple "files at root"
 * layout working as drive A while leaving the standard CP/M multi-drive
 * convention available on B+.
 *--------------------------------------------------------------------------*/
const char *const mount_paths[16] = {
    "",     "/B",   "/C",   "/D",   "/E",   "/F",   "/G",   "/H",
    "/I",   "/J",   "/K",   "/L",   "/M",   "/N",   "/O",   "/P"
};

/* Build a path: <mount[drive]>/U<user>/<name>.
   `drive` is 0..15 (A..P).  If user == 0, /U<n>/ is omitted.
   `name` may be empty / NULL for the directory itself. */
static int build_path(u8 drive, u8 user, const char *name, char *out, int max)
{
    if (drive >= 16) return -1;
    int n = 0;
    const char *p = mount_paths[drive];
    while (*p && n < max - 1) out[n++] = *p++;
    if (user > 0) {
        if (n + 4 > max - 1) return -1;
        out[n++] = '/';
        out[n++] = 'U';
        if (user >= 10) out[n++] = '0' + (user / 10);
        out[n++] = '0' + (user % 10);
    }
    /* Inject the current subpath ("/SUB1/SUB2...") if any. */
    {
        const char *sp = cur_subpath;
        while (*sp && n < max - 1) out[n++] = *sp++;
    }
    if (name && *name) {
        if (n < max - 1) out[n++] = '/';
        while (*name && n < max - 1) out[n++] = *name++;
    }
    out[n] = 0;
    return n;
}

/* Resolve "drive of this FCB": byte 0 of the FCB is 0 for current
   default drive, 1..16 for explicit A..P. */
static u8 fcb_drive(const u8 *fcb)
{
    u8 d = fcb[FCB_DRV];
    if (d == 0) return cur_drive;
    return (u8)((d - 1) & 0x0F);
}

/*-- Build a "BASE.EXT" string from a 36-byte FCB ------------------------*/
static int fcb_to_name(const u8 *fcb, char *name, int max)
{
    int n = 0;
    for (int i = 0; i < 8 && fcb[FCB_NAME + i] != ' '; i++)
        if (n < max - 1) name[n++] = (char)fcb[FCB_NAME + i];
    int has_ext = 0;
    for (int i = 0; i < 3; i++) if (fcb[FCB_NAME + 8 + i] != ' ') { has_ext = 1; break; }
    if (has_ext) {
        if (n < max - 1) name[n++] = '.';
        for (int i = 0; i < 3 && fcb[FCB_NAME + 8 + i] != ' '; i++)
            if (n < max - 1) name[n++] = (char)fcb[FCB_NAME + 8 + i];
    }
    name[n] = 0;
    return n;
}

/* Resolve an FCB to a full filesystem path through mount + user mapping. */
static int fcb_to_path(const u8 *fcb, char *out, int max)
{
    char name[16];
    fcb_to_name(fcb, name, sizeof(name));
    return build_path(fcb_drive(fcb), cur_user, name, out, max);
}

/*-- 15: Open File --------------------------------------------------------*/
static u32 d_open(u32 fcb_p, u32 b)     { (void)b;
    if (ensure_mounted() != 0) return 0xFF;
    u8 *fcb = (u8 *)fcb_p;

    char path[40];
    fcb_to_path(fcb, path, sizeof(path));

    int idx;
    if (alloc_handle(&idx) != 0) return 0xFF;
    if (fat_fopen(path, &fhandles[idx].file) != 0) {
        fhandles[idx].in_use = 0;
        return 0xFF;
    }

    stash_handle(fcb, idx);

    /* Initialize CP/M extent state from file size. */
    u32 size = fhandles[idx].file.file_size;
    u32 records = (size + 127) / 128;
    fcb[FCB_EX]  = (u8)((records >> 7) & 0x1F);
    fcb[FCB_S1]  = 0;
    fcb[FCB_S2]  = (u8)(records >> 12);
    fcb[FCB_RC]  = (u8)(records & 0x7F);
    fcb[FCB_CR]  = 0;
    return 0;       /* CP/M open returns 0..3 dirent slot; we always 0 */
}

/*-- 16: Close File -------------------------------------------------------*/
static u32 d_close(u32 fcb_p, u32 b)    { (void)b;
    u8 *fcb = (u8 *)fcb_p;
    fhandle_t *h = resolve_fcb(fcb);
    if (!h) return 0xFF;
    fat_fclose(&h->file);
    h->in_use = 0;
    fcb[FCB_PRIV+0] = 0;    /* clear magic */
    return 0;
}

/*-- Format an 8.3 name from raw 11-byte FCB form into pattern11 ----------*/
static void fcb_name_to_pattern(const u8 *raw, char *pat11)
{
    for (int i = 0; i < 11; i++) pat11[i] = (char)raw[i];
}

/* Search-state lives inside the FCB's private region for 17/18.  We use
   a single static fat_dir_iter_t since FAT_MAX_OPEN of them is huge.
   This is a phase-1 simplification: only one directory walk active at a
   time per process. */
static fat_dir_iter_t  search_iter;
static u8              search_pattern[11];
static int             search_active;

static u32 emit_dirent_to_dma(const fat_dirent_t *de)
{
    /* CP/M-style: 32-byte directory record at DMA address.
       offset 0: user (we use 0)
       offset 1..11: name
       offset 12-15: extent fields
       offset 16-31: alloc map (we don't track these meaningfully)
    */
    if (cur_dma == 0) return 0xFF;
    u8 *out = (u8 *)cur_dma;
    out[0] = 0;
    for (int i = 0; i < 11; i++) out[1 + i] = de->name[i];
    out[12] = 0;        /* ex */
    out[13] = 0;
    out[14] = 0;
    u32 records = (de->file_size + 127) / 128;
    out[15] = (u8)(records & 0x7F);
    for (int i = 16; i < 32; i++) out[i] = 0;
    return 0;
}

/*-- 17: Search First -----------------------------------------------------*/
static u32 d_search1(u32 fcb_p, u32 b)  { (void)b;
    if (ensure_mounted() != 0) return 0xFF;
    u8 *fcb = (u8 *)fcb_p;
    fcb_name_to_pattern(&fcb[FCB_NAME], (char *)search_pattern);
    for (int i = 0; i < 11; i++) {
        if (search_pattern[i] == '?') ;
        else if (search_pattern[i] >= 'a' && search_pattern[i] <= 'z')
            search_pattern[i] -= 32;
    }
    /* Open the iterator at the current drive's mount + user subdir. */
    char dir_path[40];
    build_path(fcb_drive(fcb), cur_user, 0, dir_path, sizeof(dir_path));
    if (fat_diropen_path(&search_iter, dir_path) != 0) {
        search_active = 0;
        return 0xFF;
    }
    fat_dirent_t de;
    if (fat_search_next(&search_iter, (const char *)search_pattern, &de) != 0) {
        search_active = 0;
        return 0xFF;
    }
    search_active = 1;
    if (emit_dirent_to_dma(&de) != 0) return 0xFF;
    return 0;
}

/*-- 18: Search Next ------------------------------------------------------*/
static u32 d_searchN(u32 fcb_p, u32 b)  { (void)fcb_p; (void)b;
    if (!search_active) return 0xFF;
    fat_dirent_t de;
    if (fat_search_next(&search_iter, (const char *)search_pattern, &de) != 0) {
        search_active = 0;
        return 0xFF;
    }
    if (emit_dirent_to_dma(&de) != 0) return 0xFF;
    return 0;
}

/*-- 20: Read Sequential --------------------------------------------------*/
static u32 d_read_seq(u32 fcb_p, u32 b) { (void)b;
    u8 *fcb = (u8 *)fcb_p;
    fhandle_t *h = resolve_fcb(fcb);
    if (!h)             return 1;       /* end of file / error */
    if (cur_dma == 0)   return 0xFF;

    /* Compute byte offset from extent fields */
    u32 record = ((u32)fcb[FCB_S2] << 12) | ((u32)fcb[FCB_EX] << 7) | fcb[FCB_CR];
    u32 byte_off = record * 128;
    if (byte_off >= h->file.file_size) return 1;

    if (fat_fseek(&h->file, byte_off) != 0) return 0xFF;
    u8 *dma = (u8 *)cur_dma;
    int got = fat_fread(&h->file, dma, 128);
    if (got <= 0) return 1;
    if (got < 128) {
        /* zero-pad */
        for (int i = got; i < 128; i++) dma[i] = 0x1A;     /* CP/M EOF char */
    }

    /* Advance CP/M extent counters */
    record++;
    fcb[FCB_CR] = (u8)(record & 0x7F);
    fcb[FCB_EX] = (u8)((record >> 7) & 0x1F);
    fcb[FCB_S2] = (u8)(record >> 12);
    return 0;
}

/*-- 26: Set DMA Address --------------------------------------------------*/
static u32 d_set_dma(u32 a, u32 b)      { (void)b;
    cur_dma = a;
    return 0;
}

/*-- 33: Read Random ------------------------------------------------------*/
static u32 d_read_random(u32 fcb_p, u32 b) { (void)b;
    u8 *fcb = (u8 *)fcb_p;
    fhandle_t *h = resolve_fcb(fcb);
    if (!h)             return 6;       /* invalid file */
    if (cur_dma == 0)   return 0xFF;

    u32 record = (u32)fcb[FCB_R0] | ((u32)fcb[FCB_R0 + 1] << 8) |
                 ((u32)fcb[FCB_R0 + 2] << 16);
    u32 byte_off = record * 128;
    if (byte_off >= h->file.file_size) return 1;        /* EOF */

    if (fat_fseek(&h->file, byte_off) != 0) return 0xFF;
    u8 *dma = (u8 *)cur_dma;
    int got = fat_fread(&h->file, dma, 128);
    if (got <= 0) return 1;
    if (got < 128) {
        for (int i = got; i < 128; i++) dma[i] = 0x1A;
    }

    /* Update extent fields to match. */
    fcb[FCB_CR] = (u8)(record & 0x7F);
    fcb[FCB_EX] = (u8)((record >> 7) & 0x1F);
    fcb[FCB_S2] = (u8)(record >> 12);
    return 0;
}

/*-- 35: Compute File Size ------------------------------------------------*/
static u32 d_file_size(u32 fcb_p, u32 b) { (void)b;
    u8 *fcb = (u8 *)fcb_p;
    fhandle_t *h = resolve_fcb(fcb);
    if (!h) return 0xFF;
    u32 records = (h->file.file_size + 127) / 128;
    fcb[FCB_R0]     = (u8)(records & 0xFF);
    fcb[FCB_R0 + 1] = (u8)((records >> 8) & 0xFF);
    fcb[FCB_R0 + 2] = (u8)((records >> 16) & 0xFF);
    return 0;
}

/*-- 19: Delete File ------------------------------------------------------*/
static u32 d_delete(u32 fcb_p, u32 b)   { (void)b;
    if (ensure_mounted() != 0) return 0xFF;
    u8 *fcb = (u8 *)fcb_p;
    char path[40];
    fcb_to_path(fcb, path, sizeof(path));
    return (fat_funlink(path) == 0) ? 0 : 0xFF;
}

/*-- 21: Write Sequential -------------------------------------------------*/
static u32 d_write_seq(u32 fcb_p, u32 b) { (void)b;
    u8 *fcb = (u8 *)fcb_p;
    fhandle_t *h = resolve_fcb(fcb);
    if (!h)             return 0xFF;
    if (cur_dma == 0)   return 0xFF;

    u32 record = ((u32)fcb[FCB_S2] << 12) | ((u32)fcb[FCB_EX] << 7) | fcb[FCB_CR];
    u32 byte_off = record * 128;
    if (fat_fseek(&h->file, byte_off) != 0) return 0xFF;
    int wrote = fat_fwrite(&h->file, (const void *)cur_dma, 128);
    if (wrote != 128) return 0xFF;

    record++;
    fcb[FCB_CR] = (u8)(record & 0x7F);
    fcb[FCB_EX] = (u8)((record >> 7) & 0x1F);
    fcb[FCB_S2] = (u8)(record >> 12);

    /* Update extent record-count to reflect the file's growth. */
    u32 records_total = (h->file.file_size + 127) / 128;
    fcb[FCB_RC] = (u8)(records_total & 0x7F);

    fat_fflush(&h->file);
    return 0;
}

/*-- 22: Make File --------------------------------------------------------*/
static u32 d_make(u32 fcb_p, u32 b)     { (void)b;
    if (ensure_mounted() != 0) return 0xFF;
    u8 *fcb = (u8 *)fcb_p;

    char path[40];
    fcb_to_path(fcb, path, sizeof(path));

    int idx;
    if (alloc_handle(&idx) != 0) return 0xFF;
    if (fat_fcreate(path, &fhandles[idx].file) != 0) {
        fhandles[idx].in_use = 0;
        return 0xFF;
    }
    stash_handle(fcb, idx);
    fcb[FCB_EX] = 0; fcb[FCB_S1] = 0; fcb[FCB_S2] = 0;
    fcb[FCB_RC] = 0; fcb[FCB_CR] = 0;
    return 0;
}

/*-- 23: Rename File ------------------------------------------------------*/
static u32 d_rename(u32 fcb_p, u32 b)   { (void)b;
    if (ensure_mounted() != 0) return 0xFF;
    /* CP/M rename FCB layout:
         offset 0..15  = old FCB (name in 1..11)
         offset 16..31 = new FCB (name in 17..27 of original FCB pointer) */
    u8 *fcb = (u8 *)fcb_p;

    /* Old and new go through the SAME drive (per CP/M convention). */
    u8 old_drv = fcb_drive(fcb);
    u8 base[36];
    int i;

    /* Build a synthetic FCB to reuse fcb_to_name. */
    for (i = 0; i < 36; i++) base[i] = 0;
    base[FCB_DRV] = fcb[FCB_DRV];
    for (i = 0; i < 11; i++) base[FCB_NAME + i] = fcb[1 + i];
    char old_name[16];
    fcb_to_name(base, old_name, sizeof(old_name));

    for (i = 0; i < 11; i++) base[FCB_NAME + i] = fcb[17 + i];
    char new_name[16];
    fcb_to_name(base, new_name, sizeof(new_name));

    char old_path[40], new_path[40];
    build_path(old_drv, cur_user, old_name, old_path, sizeof(old_path));
    build_path(old_drv, cur_user, new_name, new_path, sizeof(new_path));

    return (fat_frename(old_path, new_path) == 0) ? 0 : 0xFF;
}

/*-- 34: Write Random -----------------------------------------------------*/
static u32 d_write_random(u32 fcb_p, u32 b) { (void)b;
    u8 *fcb = (u8 *)fcb_p;
    fhandle_t *h = resolve_fcb(fcb);
    if (!h)             return 0xFF;
    if (cur_dma == 0)   return 0xFF;
    u32 record = (u32)fcb[FCB_R0] | ((u32)fcb[FCB_R0 + 1] << 8) |
                 ((u32)fcb[FCB_R0 + 2] << 16);
    u32 byte_off = record * 128;
    if (fat_fseek(&h->file, byte_off) != 0) return 0xFF;
    int wrote = fat_fwrite(&h->file, (const void *)cur_dma, 128);
    if (wrote != 128) return 0xFF;
    fcb[FCB_CR] = (u8)(record & 0x7F);
    fcb[FCB_EX] = (u8)((record >> 7) & 0x1F);
    fcb[FCB_S2] = (u8)(record >> 12);
    fat_fflush(&h->file);
    return 0;
}

/*-- 36: Set Random Record (from current sequential position) -------------*/
static u32 d_set_rrec(u32 fcb_p, u32 b) { (void)b;
    u8 *fcb = (u8 *)fcb_p;
    u32 record = ((u32)fcb[FCB_S2] << 12) | ((u32)fcb[FCB_EX] << 7) | fcb[FCB_CR];
    fcb[FCB_R0]     = (u8)(record & 0xFF);
    fcb[FCB_R0 + 1] = (u8)((record >> 8) & 0xFF);
    fcb[FCB_R0 + 2] = (u8)((record >> 16) & 0xFF);
    return 0;
}

/*-- Stubs for the disk/file functions until FAT32 is wired in -------------*/
static u32 d_stub_zero(u32 a, u32 b)    { (void)a;(void)b; return 0; }
static u32 d_stub_err (u32 a, u32 b)    { (void)a;(void)b; return 0xFF; }

/*-- Dispatch table --------------------------------------------------------*/
typedef u32 (*bdos_handler_t)(u32, u32);

static bdos_handler_t bdos_table[BDOS_NFUNCS] = {
    [0]   = d_reset,
    [1]   = d_conin,
    [2]   = d_conout,
    [3]   = d_stub_zero,    /* aux in */
    [4]   = d_stub_zero,    /* aux out */
    [5]   = d_stub_zero,    /* list out */
    [6]   = d_directio,
    [7]   = d_stub_zero,    /* get iobyte */
    [8]   = d_stub_zero,    /* set iobyte */
    [9]   = d_printstr,
    [10]  = d_readline,
    [11]  = d_constat,
    [12]  = d_version,
    [13]  = d_stub_zero,    /* reset disks */
    [14]  = d_seldisk,
    /* 15..23 file I/O */
    [15]  = d_open,         /* Open File */
    [16]  = d_close,        /* Close File */
    [17]  = d_search1,      /* Search First */
    [18]  = d_searchN,      /* Search Next */
    [19]  = d_delete,       /* Delete File */
    [20]  = d_read_seq,     /* Read Sequential */
    [21]  = d_write_seq,    /* Write Sequential */
    [22]  = d_make,         /* Make File */
    [23]  = d_rename,       /* Rename File */
    [24]  = d_stub_zero,    /* login vector */
    [25]  = d_getdisk,
    [26]  = d_set_dma,
    [27]  = d_stub_zero,    /* get ALV */
    [28]  = d_stub_zero, [29] = d_stub_zero,
    [30]  = d_stub_zero, [31] = d_stub_zero,
    [32]  = d_user,
    [33]  = d_read_random,
    [34]  = d_write_random,
    [35]  = d_file_size,    /* Compute File Size */
    [36]  = d_set_rrec,     /* Set Random Record */
    [40]  = d_write_random, /* Write Random with Zero Fill (same path) */
    [37]  = d_stub_zero,
    [40]  = d_stub_err,
    [47]  = d_reset,        /* chain → for now: warm-boot */
    [48]  = d_stub_zero,
    [50]  = d_directbios,
    [59]  = d_stub_err,     /* program load — wired in later */
    [62]  = d_setexc,
    [63]  = d_stub_zero,    /* free space */
    [100] = d_xcpm_ident,
    [110] = d_ext_listroot,
    [111] = d_ext_cat,
    [112] = d_ext_mount,
    [113] = d_ext_loadrun,
    [114] = d_ext_redir_begin,
    [115] = d_ext_redir_end,
    [116] = d_ext_inrd_begin,
    [117] = d_ext_inrd_end,
    [118] = d_ext_mkdir,
    [119] = d_ext_rmdir,
    [120] = d_ext_savefile,
    [121] = d_ext_loadfile,
    [122] = d_ext_scratch,
    [123] = d_ext_copy,
    [124] = d_ext_chdir,
    [125] = d_ext_getcwd,
    [127] = d_ext_lzss_print,
};

u32 bdos_dispatch(u32 func, u32 p1, u32 p2)
{
    if (func >= BDOS_NFUNCS) return 0;
    bdos_handler_t h = bdos_table[func];
    if (!h) return 0;
    return h(p1, p2);
}

u32 bdos_call(u32 func, u32 p1, u32 p2)
{
    return bdos_dispatch(func, p1, p2);
}

void bdos_init(void)
{
    cur_drive = 0;
    cur_user  = 0;
    cur_subpath[0] = 0;
#if !defined(__mips__)
    /* 68K: install BDOS on TRAP #2 (vector 34). MIPS installs its syscall
       handler separately (see port/ncd15) — apps reach BDOS via `syscall`. */
    RAM_VEC_TABLE[34] = (u32)bdos_trap_entry;
#endif
    (void)put_str;          /* silence unused */
}
