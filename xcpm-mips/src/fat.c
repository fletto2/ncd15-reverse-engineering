/*============================================================================
 * fat.c -- FAT12 / FAT16 / FAT32 read-only driver.
 *
 * Single-mount, single-sector cache, single-FAT cache.
 * Writes deferred to a later slice.
 *==========================================================================*/
#include "xcpm.h"
#include "fat.h"

static fat_mount_t M;
static u8  sec_buf[FAT_SECTOR_SIZE];
static u32 sec_buf_lba;
static u8  sec_buf_valid;

/* it->buf/it->lfn statics removed: iter shrink regressed boot.
   Reverted to per-iter buf[]/lfn[] embedded in fat_dir_iter_t.
   See CLAUDE.md "Pending: input redirection". */

/* Shared scratch sector buffer. Doubles as:
   - the FAT-table-sector cache (read_fat_sec / fat_next_clus)
   - the work buffer for every file-modify path (fcreate/fwrite/funlink/
     frename/fflush/set_entry/find_empty_dirent)
   - fat_fread's per-iteration sector buffer
   On small targets (e.g. PVS-2, 32 KB RAM) we cannot afford either a
   second 512-byte cache *or* per-function stack buffers — the supervisor
   stack overflows into the TPA/CCP BSS when fat_* calls nest. The driver
   is single-threaded; verified that no two uses are simultaneously live
   across the call graph. The cache is invalidated on every write so the
   buffer can safely be reused as scratch in between. */
static u8  fat_wbuf[FAT_SECTOR_SIZE];
#define fat_buf fat_wbuf
static u32 fat_buf_lba;
static u8  fat_buf_valid;

static fat_file_t handles[FAT_MAX_OPEN];

/*-- Endian helpers (FAT is little-endian on disk) ------------------------*/
static u16 rd16(const u8 *p) { return (u16)(p[0] | ((u16)p[1] << 8)); }
static u32 rd32(const u8 *p)
{ return  (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

/*-- Sector cache ---------------------------------------------------------*/
static int read_sec(u32 lba, u8 *dst)
{
    if (sec_buf_valid && sec_buf_lba == lba) {
        for (int i = 0; i < FAT_SECTOR_SIZE; i++) dst[i] = sec_buf[i];
        return 0;
    }
    if (blk_read(lba, 1, sec_buf) != 0) return -1;
    sec_buf_lba = lba; sec_buf_valid = 1;
    for (int i = 0; i < FAT_SECTOR_SIZE; i++) dst[i] = sec_buf[i];
    return 0;
}

static int read_fat_sec(u32 lba)
{
    if (fat_buf_valid && fat_buf_lba == lba) return 0;
    if (blk_read(lba, 1, fat_buf) != 0) return -1;
    fat_buf_lba = lba; fat_buf_valid = 1;
    return 0;
}

/* Any code path that scribbles into fat_wbuf (file-modify path, etc.)
   must invalidate the FAT-cache view before returning to a caller that
   might next call fat_next_clus(). All such paths funnel through
   write_sec(), which invalidates fat_buf_valid below. fat_fcreate /
   fat_funlink / etc. read fresh data into fat_wbuf themselves so they
   don't need to consult the cache flag. */

/*-- Mount ----------------------------------------------------------------*/
int fat_mount(void)
{
    M.valid = 0;
    sec_buf_valid = 0; fat_buf_valid = 0;
    for (int i = 0; i < FAT_MAX_OPEN; i++) handles[i].in_use = 0;

    if (blk_init() != 0) return -1;

    u8 bpb[FAT_SECTOR_SIZE];
    if (read_sec(0, bpb) != 0) return -1;
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) return -2;

    M.bytes_per_sec  = rd16(&bpb[11]);
    M.sec_per_clus   = bpb[13];
    M.rsvd_sec       = rd16(&bpb[14]);
    M.num_fats       = bpb[16];
    M.root_ent_count = rd16(&bpb[17]);
    u32 totsec16     = rd16(&bpb[19]);
    M.fat_sec        = rd16(&bpb[22]);
    u32 totsec32     = rd32(&bpb[32]);
    M.total_sec      = totsec16 ? totsec16 : totsec32;

    if (M.bytes_per_sec != FAT_SECTOR_SIZE) return -3;
    if (M.sec_per_clus == 0)                return -4;
    if (M.num_fats == 0)                    return -5;

    if (M.fat_sec == 0) {
        /* FAT32: extended BPB */
        M.fat_sec   = rd32(&bpb[36]);
        M.root_clus = rd32(&bpb[44]);
    } else {
        M.root_clus = 0;
    }

    u32 root_dir_sec = (M.root_ent_count * 32 + M.bytes_per_sec - 1)
                       / M.bytes_per_sec;
    u32 fat_total = M.num_fats * M.fat_sec;
    M.first_data_sec = M.rsvd_sec + fat_total + root_dir_sec;
    M.first_root_sec = M.rsvd_sec + fat_total;

    u32 data_sec   = M.total_sec - M.first_data_sec;
    M.cluster_count = data_sec / M.sec_per_clus;

    if      (M.cluster_count <  4085)  M.type = FAT12;
    else if (M.cluster_count < 65525)  M.type = FAT16;
    else                               M.type = FAT32;

    M.valid = 1;
    return 0;
}

const fat_mount_t *fat_get_mount(void) { return &M; }

u32 fat_clus_to_lba(u32 clus)
{
    if (clus < 2) return 0;
    return M.first_data_sec + (clus - 2) * M.sec_per_clus;
}

/*-- FAT walk -------------------------------------------------------------*/
u32 fat_next_clus(u32 clus)
{
    u32 entry = 0;
    if (M.type == FAT16) {
        u32 off = clus * 2;
        u32 lba = M.rsvd_sec + off / FAT_SECTOR_SIZE;
        if (read_fat_sec(lba) != 0) return 0xFFFFFFFFu;
        entry = rd16(&fat_buf[off & (FAT_SECTOR_SIZE - 1)]);
        if (entry >= 0xFFF8) return 0xFFFFFFFFu;
    } else if (M.type == FAT32) {
        u32 off = clus * 4;
        u32 lba = M.rsvd_sec + off / FAT_SECTOR_SIZE;
        if (read_fat_sec(lba) != 0) return 0xFFFFFFFFu;
        entry = rd32(&fat_buf[off & (FAT_SECTOR_SIZE - 1)]) & 0x0FFFFFFFu;
        if (entry >= 0x0FFFFFF8u) return 0xFFFFFFFFu;
    } else {        /* FAT12 */
        u32 off = clus + (clus / 2);
        u32 lba = M.rsvd_sec + off / FAT_SECTOR_SIZE;
        if (read_fat_sec(lba) != 0) return 0xFFFFFFFFu;
        u32 idx = off & (FAT_SECTOR_SIZE - 1);
        u32 b0 = fat_buf[idx];
        u32 b1;
        if (idx == FAT_SECTOR_SIZE - 1) {
            /* spans across sector boundary */
            if (read_fat_sec(lba + 1) != 0) return 0xFFFFFFFFu;
            b1 = fat_buf[0];
            /* Re-read first sector as needed -- our cache only holds one,
               so for FAT12 boundary cases we double-fetch each call.
               (Boundary entries are rare; cost is negligible.) */
            if (read_fat_sec(lba) != 0) return 0xFFFFFFFFu;
            b0 = fat_buf[idx];
            if (read_fat_sec(lba + 1) != 0) return 0xFFFFFFFFu;
            b1 = fat_buf[0];
        } else {
            b1 = fat_buf[idx + 1];
        }
        if (clus & 1) entry = ((b0 >> 4) | (b1 << 4)) & 0x0FFF;
        else          entry = (b0 | (b1 << 8))        & 0x0FFF;
        if (entry >= 0xFF8) return 0xFFFFFFFFu;
    }
    return entry;
}

/*-- LFN reassembly -------------------------------------------------------*/
/* LFN entries appear before the 8.3 entry, in reverse order (highest
   sequence number first).  Each carries 13 UCS-2 chars (we narrow to
   ASCII; non-ASCII becomes '?'). */
static int lfn_collect_entry(u8 *raw, char *lfn_out, u32 *lfn_len, int max)
{
    u8 seq = raw[0];
    int last = (seq & 0x40) != 0;
    int idx  = (seq & 0x1F) - 1;
    if (idx < 0) return -1;
    int base = idx * 13;
    /* Positions of the 13 chars in the raw 32-byte LFN entry: */
    static const u8 off[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
    char tmp[13];
    int n = 0;
    for (int i = 0; i < 13; i++) {
        u16 ch = (u16)raw[off[i]] | ((u16)raw[off[i] + 1] << 8);
        if (ch == 0 || ch == 0xFFFF) break;
        tmp[n++] = (ch < 0x80) ? (char)ch : '?';
    }
    /* Place in lfn_out at position base..base+n */
    for (int i = 0; i < n && base + i < max - 1; i++) {
        lfn_out[base + i] = tmp[i];
    }
    if (last) {
        /* Length = base + n on the last entry (which has highest idx). */
        if (base + n > (int)*lfn_len) *lfn_len = base + n;
    }
    /* For intermediate entries, ensure length includes them. */
    if (base + 13 > (int)*lfn_len && !last) *lfn_len = base + 13;
    return 0;
}

/*-- Directory iteration --------------------------------------------------*/
static int dir_load_sector(fat_dir_iter_t *it, u32 lba)
{
    if (it->buf_lba_valid && it->buf_lba == lba) return 0;
    if (blk_read(lba, 1, it->buf) != 0) return -1;
    it->buf_lba = lba; it->buf_lba_valid = 1;
    return 0;
}

int fat_diropen_root(fat_dir_iter_t *it)
{
    if (!M.valid) return -1;
    if (M.type == FAT32) {
        return fat_diropen_clus(it, M.root_clus);
    }
    it->cur_clus = 0;     /* FAT12/16 fixed-area root */
    it->cur_lba  = M.first_root_sec;
    it->ent_idx  = 0;
    it->buf_lba_valid = 0;
    it->lfn[0] = 0; it->lfn_len = 0;
    return 0;
}

/* Forward decls (definitions later in this file). */
static int name_match_83(const u8 *raw11, const char *want);
static int strieq_(const char *a, const char *b);

/* Open the directory at `path` (e.g. "/B" or "/A/U5" or "" for root). */
int fat_diropen_path(fat_dir_iter_t *it, const char *path)
{
    if (!M.valid) return -1;
    if (path == 0 || path[0] == 0 ||
        (path[0] == '/' && path[1] == 0)) {
        return fat_diropen_root(it);
    }
    if (path[0] == '/') path++;

    /* Walk components, descending into each as a directory. */
    if (fat_diropen_root(it) != 0) return -1;
    char comp[16];
    fat_dirent_t de;
    while (*path) {
        int n = 0;
        while (*path && *path != '/' && n < (int)sizeof(comp) - 1) {
            comp[n++] = *path++;
        }
        comp[n] = 0;
        if (*path == '/') path++;
        int found = 0;
        char lfn[256];
        while (fat_dirnext(it, &de, lfn, sizeof(lfn)) == 0) {
            if (name_match_83(de.name, comp) ||
                (lfn[0] && strieq_(lfn, comp))) { found = 1; break; }
        }
        if (!found) return -2;
        if (!(de.attr & FAT_ATTR_DIR)) return -3;
        u32 child = ((u32)de.first_clus_hi << 16) | de.first_clus_lo;
        if (fat_diropen_clus(it, child) != 0) return -1;
    }
    return 0;
}

int fat_diropen_clus(fat_dir_iter_t *it, u32 clus)
{
    if (!M.valid) return -1;
    it->cur_clus = clus;
    it->cur_lba  = fat_clus_to_lba(clus);
    it->ent_idx  = 0;
    it->buf_lba_valid = 0;
    it->lfn[0] = 0; it->lfn_len = 0;
    return 0;
}

int fat_dirnext(fat_dir_iter_t *it, fat_dirent_t *out, char *lfn_out, int lfn_max)
{
    /* Each iteration returns the next non-LFN, non-deleted, non-volume
       directory entry, with any preceding LFN reassembled into lfn_out. */
    if (!M.valid) return -1;
    if (lfn_out) lfn_out[0] = 0;
    it->lfn_len = 0;
    /* zero the LFN scratch */
    for (u32 i = 0; i < sizeof(it->lfn); i++) it->lfn[i] = 0;

    for (;;) {
        if (it->cur_lba == 0) return -1;
        if (dir_load_sector(it, it->cur_lba) != 0) return -1;

        u32 ents_per_sec = FAT_SECTOR_SIZE / 32;
        if (it->ent_idx >= ents_per_sec) {
            /* advance to next sector */
            u32 cur_sec_in_clus = (it->cur_lba - fat_clus_to_lba(it->cur_clus));
            if (it->cur_clus == 0) {
                /* FAT12/16 root */
                u32 max = (M.root_ent_count * 32) / FAT_SECTOR_SIZE;
                if (it->cur_lba - M.first_root_sec + 1 >= max) return 1;
                it->cur_lba++;
            } else {
                if (cur_sec_in_clus + 1 < M.sec_per_clus) {
                    it->cur_lba++;
                } else {
                    u32 nxt = fat_next_clus(it->cur_clus);
                    if (nxt == 0xFFFFFFFFu) return 1;
                    it->cur_clus = nxt;
                    it->cur_lba  = fat_clus_to_lba(nxt);
                }
            }
            it->ent_idx = 0;
            it->buf_lba_valid = 0;
            continue;
        }
        u8 *e = &it->buf[it->ent_idx * 32];
        u8 first = e[0];
        u8 attr  = e[11];
        if (first == 0x00) return 1;        /* end of dir */
        if (first == 0xE5) {                /* deleted */
            it->ent_idx++;
            it->lfn_len = 0;
            for (u32 i = 0; i < sizeof(it->lfn); i++) it->lfn[i] = 0;
            continue;
        }
        if (attr == FAT_ATTR_LFN) {
            lfn_collect_entry(e, it->lfn, &it->lfn_len, sizeof(it->lfn));
            it->ent_idx++;
            continue;
        }
        if (attr & FAT_ATTR_VOLID) {
            it->ent_idx++;
            it->lfn_len = 0;
            continue;
        }

        /* Real 8.3 entry -- convert from on-disk little-endian to
           native byte order. */
        for (int i = 0; i < 11; i++) out->name[i] = e[i];
        out->attr        = e[11];
        out->ntres       = e[12];
        out->ctime_tenth = e[13];
        out->ctime         = rd16(&e[14]);
        out->cdate         = rd16(&e[16]);
        out->adate         = rd16(&e[18]);
        out->first_clus_hi = rd16(&e[20]);
        out->wtime         = rd16(&e[22]);
        out->wdate         = rd16(&e[24]);
        out->first_clus_lo = rd16(&e[26]);
        out->file_size     = rd32(&e[28]);
        u32 dir_lba_here = it->cur_lba;
        u32 dir_off_here = it->ent_idx * 32;
        (void)dir_lba_here; (void)dir_off_here;
        if (lfn_out) {
            if (it->lfn_len > 0 && (u32)lfn_max > it->lfn_len) {
                int j;
                for (j = 0; j < (int)it->lfn_len && j < lfn_max - 1; j++)
                    lfn_out[j] = it->lfn[j];
                lfn_out[j] = 0;
            } else {
                lfn_out[0] = 0;     /* no LFN */
            }
        }
        it->ent_idx++;
        it->lfn_len = 0;
        for (u32 i = 0; i < sizeof(it->lfn); i++) it->lfn[i] = 0;
        return 0;
    }
}

/*-- File open / read -----------------------------------------------------*/
static int name_match_83(const u8 *raw11, const char *want)
{
    /* `want` is a CP/M-style 8.3 name like "FOO.TXT" (uppercase).
       `raw11` is the on-disk space-padded "FOO     TXT" form. */
    char base[8], ext[3];
    int i;
    for (i = 0; i < 8 && raw11[i] != ' '; i++) base[i] = (char)raw11[i];
    int blen = i;
    for (i = 0; i < 3 && raw11[8 + i] != ' '; i++) ext[i] = (char)raw11[8 + i];
    int elen = i;

    /* Build a candidate "BASE.EXT" string */
    char cand[16]; int n = 0;
    for (i = 0; i < blen; i++) cand[n++] = base[i];
    if (elen > 0) {
        cand[n++] = '.';
        for (i = 0; i < elen; i++) cand[n++] = ext[i];
    }
    cand[n] = 0;

    /* Compare case-insensitively */
    int wi = 0;
    while (wi < n && want[wi]) {
        char a = cand[wi], b = want[wi];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
        wi++;
    }
    return wi == n && want[wi] == 0;
}

static int strieq_(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

/* Find an entry by name in `dir_iter` (already opened to a directory).
   Returns 0 + populates `out_de` on hit, 1 if not found. */
static int find_entry_in_dir(fat_dir_iter_t *it, const char *name,
                             fat_dirent_t *out_de)
{
    char lfn[256];
    while (fat_dirnext(it, out_de, lfn, sizeof(lfn)) == 0) {
        if (name_match_83(out_de->name, name) ||
            (lfn[0] && strieq_(lfn, name))) {
            return 0;
        }
    }
    return 1;
}

/* Path-walking open: accepts paths like "FOO.TXT", "/FOO.TXT",
   "DOCS/FOO.TXT", "/A/B/C.TXT" -- traverses subdirs as needed. */
int fat_fopen(const char *path, fat_file_t *fp)
{
    if (!M.valid) return -1;
    if (path[0] == '/') path++;

    /* Walk components.  Each component is a directory entry except the
       last, which is the file. */
    fat_dir_iter_t it;
    if (fat_diropen_root(&it) != 0) return -1;

    char comp[16];      /* one path component (8.3 fits in 12 chars) */
    fat_dirent_t de;

    while (*path) {
        /* Extract next component up to '/' or end. */
        int n = 0;
        while (*path && *path != '/' && n < (int)sizeof(comp) - 1) {
            comp[n++] = *path++;
        }
        comp[n] = 0;
        int more = (*path == '/');
        if (more) path++;

        if (find_entry_in_dir(&it, comp, &de) != 0) return -2;

        if (more) {
            /* Must be a directory; descend. */
            if (!(de.attr & FAT_ATTR_DIR)) return -3;
            u32 child = ((u32)de.first_clus_hi << 16) | de.first_clus_lo;
            if (fat_diropen_clus(&it, child) != 0) return -1;
        } else {
            /* Final component: must be a regular file. */
            if (de.attr & FAT_ATTR_DIR) return -4;
            fp->in_use     = 1;
            fp->first_clus = ((u32)de.first_clus_hi << 16) | de.first_clus_lo;
            fp->cur_clus   = fp->first_clus;
            fp->cur_pos    = 0;
            fp->file_size  = de.file_size;
            fp->attr       = de.attr;
            return 0;
        }
    }
    return -2;
}

int fat_fread(fat_file_t *fp, void *buf, u32 max)
{
    if (!fp->in_use) return -1;
    if (fp->cur_pos >= fp->file_size) return 0;
    u32 remaining = fp->file_size - fp->cur_pos;
    if (max > remaining) max = remaining;

    u8 *dst = (u8 *)buf;
    u32 produced = 0;
    while (produced < max) {
        u32 clus_size = M.sec_per_clus * FAT_SECTOR_SIZE;
        u32 in_clus_pos = fp->cur_pos & (clus_size - 1);
        u32 sec_in_clus = in_clus_pos / FAT_SECTOR_SIZE;
        u32 in_sec_pos  = in_clus_pos & (FAT_SECTOR_SIZE - 1);

        u32 lba = fat_clus_to_lba(fp->cur_clus) + sec_in_clus;
        fat_buf_valid = 0;
        if (read_sec(lba, fat_wbuf) != 0) return -1;

        u32 chunk = FAT_SECTOR_SIZE - in_sec_pos;
        if (chunk > max - produced) chunk = max - produced;
        for (u32 i = 0; i < chunk; i++) dst[produced + i] = fat_wbuf[in_sec_pos + i];
        produced += chunk;
        fp->cur_pos += chunk;

        /* Advance cluster if we crossed the boundary. */
        if ((fp->cur_pos & (clus_size - 1)) == 0 && produced < max) {
            u32 nxt = fat_next_clus(fp->cur_clus);
            if (nxt == 0xFFFFFFFFu) break;
            fp->cur_clus = nxt;
        }
    }
    return (int)produced;
}

void fat_fclose(fat_file_t *fp)
{
    fp->in_use = 0;
}

u32 fat_fsize(fat_file_t *fp) { return fp->in_use ? fp->file_size : 0; }

/* Seek to absolute byte position; rewalks the FAT chain from first_clus. */
int fat_fseek(fat_file_t *fp, u32 byte_pos)
{
    if (!fp->in_use) return -1;
    if (byte_pos > fp->file_size) byte_pos = fp->file_size;
    fp->cur_pos = byte_pos;
    fp->cur_clus = fp->first_clus;
    u32 clus_size = M.sec_per_clus * FAT_SECTOR_SIZE;
    u32 clus_to_skip = byte_pos / clus_size;
    while (clus_to_skip-- > 0) {
        u32 nxt = fat_next_clus(fp->cur_clus);
        if (nxt == 0xFFFFFFFFu) return -1;
        fp->cur_clus = nxt;
    }
    return 0;
}

/*-- Pattern match with ? wildcards on raw 11-byte 8.3 name --------------*/
int fat_match_83(const u8 *raw11, const char *pattern)
{
    /* pattern is also 11 bytes, space-padded.  '?' = any char. */
    for (int i = 0; i < 11; i++) {
        char p = pattern[i];
        u8 r = raw11[i];
        if (p == '?') continue;
        if (p != r) return 0;
    }
    return 1;
}

int fat_search_first(fat_dir_iter_t *it, const char *pattern11,
                     fat_dirent_t *out)
{
    if (fat_diropen_root(it) != 0) return -1;
    return fat_search_next(it, pattern11, out);
}

int fat_search_next(fat_dir_iter_t *it, const char *pattern11,
                    fat_dirent_t *out)
{
    char lfn[256];
    for (;;) {
        int rc = fat_dirnext(it, out, lfn, sizeof(lfn));
        if (rc != 0) return rc;
        if (out->attr & FAT_ATTR_VOLID) continue;
        if (fat_match_83(out->name, pattern11)) return 0;
    }
}

/*============================================================================
 * Write path
 *==========================================================================*/

/*-- Endian helpers (write LE on disk) ------------------------------------*/
static void wr16(u8 *p, u16 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void wr32(u8 *p, u32 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
                                 p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }

/*-- Write a sector (also updates the sec_buf cache if it matches) --------*/
static int write_sec(u32 lba, const u8 *src)
{
    if (blk_write(lba, 1, src) != 0) return -1;
    /* Invalidate caches for this LBA so we re-read fresh next time. */
    if (sec_buf_valid && sec_buf_lba == lba) sec_buf_valid = 0;
    if (fat_buf_valid && fat_buf_lba == lba) fat_buf_valid = 0;
    return 0;
}

/*-- Set FAT entry for cluster `clus` to `value`, writing back to all
    redundant FAT copies (typically 2). ------------------------------*/
static int fat_set_entry(u32 clus, u32 value)
{
    u32 off, lba;
    u8  *buf = fat_wbuf;
    fat_buf_valid = 0;      /* about to overwrite the shared buffer */

    if (M.type == FAT16) {
        off = clus * 2;
    } else if (M.type == FAT32) {
        off = clus * 4;
    } else {
        off = clus + (clus / 2);    /* FAT12 */
    }
    lba = M.rsvd_sec + off / FAT_SECTOR_SIZE;
    if (blk_read(lba, 1, buf) != 0) return -1;

    u32 idx = off & (FAT_SECTOR_SIZE - 1);

    if (M.type == FAT16) {
        wr16(&buf[idx], (u16)value);
    } else if (M.type == FAT32) {
        u32 cur = rd32(&buf[idx]) & 0xF0000000u;
        wr32(&buf[idx], cur | (value & 0x0FFFFFFFu));
    } else {
        /* FAT12: 12-bit nibble-packed, possibly crossing sector boundary */
        if (clus & 1) {
            /* high nibble of buf[idx] (bits 4..7) + buf[idx+1] full */
            u8 b0 = buf[idx];
            buf[idx] = (b0 & 0x0F) | (u8)((value & 0x0F) << 4);
            if (idx == FAT_SECTOR_SIZE - 1) {
                /* second byte spans next sector */
                if (write_sec(lba, buf) != 0) return -1;
                if (blk_read(lba + 1, 1, buf) != 0) return -1;
                buf[0] = (u8)((value >> 4) & 0xFF);
                if (write_sec(lba + 1, buf) != 0) return -1;
                /* mirror to second FAT */
                lba += M.fat_sec;
                if (blk_read(lba, 1, buf) != 0) return -1;
                buf[idx] = (b0 & 0x0F) | (u8)((value & 0x0F) << 4);
                if (write_sec(lba, buf) != 0) return -1;
                if (blk_read(lba + 1, 1, buf) != 0) return -1;
                buf[0] = (u8)((value >> 4) & 0xFF);
                return write_sec(lba + 1, buf);
            }
            buf[idx + 1] = (u8)((value >> 4) & 0xFF);
        } else {
            /* low byte = value low 8, high nibble of buf[idx+1] = value high 4 */
            buf[idx] = (u8)(value & 0xFF);
            if (idx == FAT_SECTOR_SIZE - 1) {
                if (write_sec(lba, buf) != 0) return -1;
                if (blk_read(lba + 1, 1, buf) != 0) return -1;
                u8 hi = buf[0];
                buf[0] = (hi & 0xF0) | (u8)((value >> 8) & 0x0F);
                if (write_sec(lba + 1, buf) != 0) return -1;
                lba += M.fat_sec;
                if (blk_read(lba, 1, buf) != 0) return -1;
                buf[idx] = (u8)(value & 0xFF);
                if (write_sec(lba, buf) != 0) return -1;
                if (blk_read(lba + 1, 1, buf) != 0) return -1;
                hi = buf[0];
                buf[0] = (hi & 0xF0) | (u8)((value >> 8) & 0x0F);
                return write_sec(lba + 1, buf);
            }
            u8 hi = buf[idx + 1];
            buf[idx + 1] = (hi & 0xF0) | (u8)((value >> 8) & 0x0F);
        }
    }

    /* Write back primary FAT */
    if (write_sec(lba, buf) != 0) return -1;
    /* Mirror to redundant FAT(s) */
    for (u32 i = 1; i < M.num_fats; i++) {
        u32 m_lba = lba + i * M.fat_sec;
        if (write_sec(m_lba, buf) != 0) return -1;
    }
    return 0;
}

/*-- Count free clusters by walking the FAT.  No directory traversal, so
    safe to call from any BDOS context regardless of stack pressure. */
u32 fat_count_free_clusters(void)
{
    if (!M.valid) return 0;
    u32 max = M.cluster_count + 2;
    u32 free_count = 0;
    for (u32 c = 2; c < max; c++) {
        if (fat_next_clus(c) == 0) free_count++;
    }
    return free_count;
}

/*-- Allocate a free cluster, mark it as EOC, return its number (>=2) ----*/
static u32 fat_alloc_cluster(void)
{
    u32 max = M.cluster_count + 2;
    for (u32 c = 2; c < max; c++) {
        u32 v = fat_next_clus(c);
        if (v == 0) {
            /* Mark as EOC. */
            u32 eoc = (M.type == FAT12) ? 0xFFF
                    : (M.type == FAT16) ? 0xFFFF
                    : 0x0FFFFFFFu;
            if (fat_set_entry(c, eoc) != 0) return 0;
            return c;
        }
    }
    return 0;       /* disk full */
}

/*-- Free a cluster chain ------------------------------------------------*/
static int fat_free_chain(u32 first)
{
    u32 c = first;
    while (c >= 2 && c < M.cluster_count + 2) {
        u32 nxt = fat_next_clus(c);
        if (fat_set_entry(c, 0) != 0) return -1;
        if (nxt == 0xFFFFFFFFu) break;
        c = nxt;
    }
    return 0;
}

/*-- Walk the root directory looking for `name11` (11-byte raw form).
    Returns 0 + populates dir_lba/dir_off if found, 1 if not found. */
static int fat_find_dirent(const char *name11, u32 *dir_lba, u32 *dir_off)
{
    fat_dir_iter_t it;
    if (fat_diropen_root(&it) != 0) return -1;
    fat_dirent_t de;
    char lfn[256];
    while (fat_dirnext(&it, &de, lfn, sizeof(lfn)) == 0) {
        if (de.attr & FAT_ATTR_VOLID) continue;
        int match = 1;
        for (int i = 0; i < 11; i++)
            if (de.name[i] != (u8)name11[i]) { match = 0; break; }
        if (match) {
            *dir_lba = it.cur_lba;
            *dir_off = (it.ent_idx - 1) * 32;
            return 0;
        }
    }
    return 1;
}

/*-- Find an empty dirent slot in root and return its location. ----------*/
static int fat_find_empty_dirent(u32 *dir_lba, u32 *dir_off)
{
    fat_dir_iter_t it;
    if (fat_diropen_root(&it) != 0) return -1;
    fat_buf_valid = 0;

    /* Walk every sector, scanning all 16 dirents per sector for first byte
       == 0x00 (never used) or 0xE5 (deleted). */
    if (M.type != FAT32) {
        u32 max_sec = (M.root_ent_count * 32) / FAT_SECTOR_SIZE;
        for (u32 s = 0; s < max_sec; s++) {
            u32 lba = M.first_root_sec + s;
            if (blk_read(lba, 1, fat_wbuf) != 0) return -1;
            for (u32 e = 0; e < FAT_SECTOR_SIZE / 32; e++) {
                u8 first = fat_wbuf[e * 32];
                if (first == 0x00 || first == 0xE5) {
                    *dir_lba = lba;
                    *dir_off = e * 32;
                    return 0;
                }
            }
        }
    } else {
        /* FAT32: walk cluster chain, allocate new cluster if full. */
        u32 c = M.root_clus;
        while (c >= 2) {
            for (u32 s = 0; s < M.sec_per_clus; s++) {
                u32 lba = fat_clus_to_lba(c) + s;
                if (blk_read(lba, 1, fat_wbuf) != 0) return -1;
                for (u32 e = 0; e < FAT_SECTOR_SIZE / 32; e++) {
                    u8 first = fat_wbuf[e * 32];
                    if (first == 0x00 || first == 0xE5) {
                        *dir_lba = lba;
                        *dir_off = e * 32;
                        return 0;
                    }
                }
            }
            u32 nxt = fat_next_clus(c);
            if (nxt == 0xFFFFFFFFu) {
                /* Extend root: allocate new cluster, link in. */
                u32 nc = fat_alloc_cluster();
                if (nc == 0) return -1;
                if (fat_set_entry(c, nc) != 0) return -1;
                /* Zero the new cluster. */
                for (int i = 0; i < FAT_SECTOR_SIZE; i++) fat_wbuf[i] = 0;
                for (u32 s = 0; s < M.sec_per_clus; s++)
                    write_sec(fat_clus_to_lba(nc) + s, fat_wbuf);
                c = nc;
                continue;
            }
            c = nxt;
        }
    }
    return 1;       /* dir full */
}

/*-- Build a 11-byte raw FAT name from "FOO.TXT" form. -------------------*/
static void build_raw_name(const char *name, char raw[11])
{
    int i;
    for (i = 0; i < 11; i++) raw[i] = ' ';
    int j = 0;
    while (*name && *name != '.' && j < 8) {
        char c = *name++;
        if (c >= 'a' && c <= 'z') c -= 32;
        raw[j++] = c;
    }
    if (*name == '.') name++;
    j = 8;
    while (*name && j < 11) {
        char c = *name++;
        if (c >= 'a' && c <= 'z') c -= 32;
        raw[j++] = c;
    }
}

/*-- fat_fcreate: create or truncate a file ------------------------------*/
int fat_fcreate(const char *path, fat_file_t *fp)
{
    if (!M.valid) return -1;
    fat_buf_valid = 0;
    if (path[0] == '/') path++;

    char raw[11];
    build_raw_name(path, raw);

    u32 dir_lba, dir_off;
    int found = fat_find_dirent(raw, &dir_lba, &dir_off);
    if (found < 0) return -1;
    if (found == 0) {
        /* Existing file: free its chain. fat_free_chain uses fat_wbuf
           internally (via fat_set_entry), so call it BEFORE we read the
           dirent sector into fat_wbuf. */
        if (blk_read(dir_lba, 1, fat_wbuf) != 0) return -1;
        u32 first_clus = ((u32)rd16(&fat_wbuf[dir_off + 20]) << 16)
                       |  rd16(&fat_wbuf[dir_off + 26]);
        if (first_clus >= 2) fat_free_chain(first_clus);
        if (blk_read(dir_lba, 1, fat_wbuf) != 0) return -1;
        u8 *e = &fat_wbuf[dir_off];
        u32 fc2 = ((u32)rd16(&e[20]) << 16) | rd16(&e[26]);
        (void)fc2;
        wr32(&e[28], 0);
        wr16(&e[20], 0);
        wr16(&e[26], 0);
        e[11] = 0x20;
        if (write_sec(dir_lba, fat_wbuf) != 0) return -1;
    } else {
        if (fat_find_empty_dirent(&dir_lba, &dir_off) != 0) return -1;
        if (blk_read(dir_lba, 1, fat_wbuf) != 0) return -1;
        u8 *e = &fat_wbuf[dir_off];
        for (int i = 0; i < 32; i++) e[i] = 0;
        for (int i = 0; i < 11; i++) e[i] = (u8)raw[i];
        e[11] = 0x20;
        if (write_sec(dir_lba, fat_wbuf) != 0) return -1;
    }

    /* Initialize handle.  No clusters allocated until first fat_fwrite. */
    fp->in_use     = 1;
    fp->first_clus = 0;
    fp->cur_clus   = 0;
    fp->cur_pos    = 0;
    fp->file_size  = 0;
    fp->dir_lba    = dir_lba;
    fp->dir_off    = dir_off;
    fp->attr       = 0x20;
    return 0;
}

/*-- fat_fwrite: append bytes, allocating clusters as needed -------------*/
int fat_fwrite(fat_file_t *fp, const void *buf, u32 n)
{
    if (!fp->in_use || !M.valid) return -1;
    fat_buf_valid = 0;
    const u8 *src = (const u8 *)buf;
    u32 written = 0;
    u32 clus_size = M.sec_per_clus * FAT_SECTOR_SIZE;

    while (written < n) {
        /* Need a current cluster?  Allocate one if the file has none yet. */
        if (fp->first_clus == 0) {
            u32 nc = fat_alloc_cluster();
            if (nc == 0) return (int)written;
            fp->first_clus = nc;
            fp->cur_clus   = nc;
            /* Update dirent's first-cluster fields. */
            if (blk_read(fp->dir_lba, 1, fat_wbuf) != 0) return -1;
            u8 *e = &fat_wbuf[fp->dir_off];
            wr16(&e[26], (u16)(nc & 0xFFFF));
            wr16(&e[20], (u16)((nc >> 16) & 0xFFFF));
            if (write_sec(fp->dir_lba, fat_wbuf) != 0) return -1;
        }

        u32 in_clus_pos = fp->cur_pos & (clus_size - 1);
        u32 sec_in_clus = in_clus_pos / FAT_SECTOR_SIZE;
        u32 in_sec_pos  = in_clus_pos & (FAT_SECTOR_SIZE - 1);
        u32 lba = fat_clus_to_lba(fp->cur_clus) + sec_in_clus;

        /* Read-modify-write the target sector. */
        if (blk_read(lba, 1, fat_wbuf) != 0) return -1;

        u32 chunk = FAT_SECTOR_SIZE - in_sec_pos;
        if (chunk > n - written) chunk = n - written;
        for (u32 i = 0; i < chunk; i++) fat_wbuf[in_sec_pos + i] = src[written + i];
        if (write_sec(lba, fat_wbuf) != 0) return -1;

        written += chunk;
        fp->cur_pos += chunk;
        if (fp->cur_pos > fp->file_size) fp->file_size = fp->cur_pos;

        /* Crossed cluster boundary? Allocate next cluster if needed. */
        if ((fp->cur_pos & (clus_size - 1)) == 0 && written < n) {
            u32 nxt = fat_next_clus(fp->cur_clus);
            if (nxt == 0xFFFFFFFFu) {
                /* Allocate a new cluster and link it. */
                u32 nc = fat_alloc_cluster();
                if (nc == 0) return (int)written;
                if (fat_set_entry(fp->cur_clus, nc) != 0) return -1;
                fp->cur_clus = nc;
            } else {
                fp->cur_clus = nxt;
            }
        }
    }

    return (int)written;
}

/*-- fat_fflush: write back the size field to the dirent -----------------*/
int fat_fflush(fat_file_t *fp)
{
    if (!fp->in_use || !M.valid) return -1;
    fat_buf_valid = 0;
    if (blk_read(fp->dir_lba, 1, fat_wbuf) != 0) return -1;
    u8 *e = &fat_wbuf[fp->dir_off];
    wr32(&e[28], fp->file_size);
    /* Update first-cluster fields in case writes attached one. */
    if (fp->first_clus) {
        wr16(&e[26], (u16)(fp->first_clus & 0xFFFF));
        wr16(&e[20], (u16)((fp->first_clus >> 16) & 0xFFFF));
    }
    return write_sec(fp->dir_lba, fat_wbuf);
}

/*-- fat_funlink: free the cluster chain and mark the dirent deleted -----*/
int fat_funlink(const char *path)
{
    if (!M.valid) return -1;
    fat_buf_valid = 0;
    if (path[0] == '/') path++;
    char raw[11];
    build_raw_name(path, raw);
    u32 dir_lba, dir_off;
    if (fat_find_dirent(raw, &dir_lba, &dir_off) != 0) return -1;
    /* Read dirent, capture first cluster, free chain (which uses fat_wbuf
       internally), then re-read and mark deleted. */
    if (blk_read(dir_lba, 1, fat_wbuf) != 0) return -1;
    u32 first_clus = ((u32)rd16(&fat_wbuf[dir_off + 20]) << 16)
                   |  rd16(&fat_wbuf[dir_off + 26]);
    if (first_clus >= 2) fat_free_chain(first_clus);
    if (blk_read(dir_lba, 1, fat_wbuf) != 0) return -1;
    fat_wbuf[dir_off] = 0xE5;
    return write_sec(dir_lba, fat_wbuf);
}

/*-- fat_frename: rename a file (root-only, simple form) -----------------*/
int fat_frename(const char *old_path, const char *new_path)
{
    if (!M.valid) return -1;
    fat_buf_valid = 0;
    if (old_path[0] == '/') old_path++;
    if (new_path[0] == '/') new_path++;
    char old_raw[11], new_raw[11];
    build_raw_name(old_path, old_raw);
    build_raw_name(new_path, new_raw);

    /* Refuse if new name already exists. */
    u32 lba_dummy, off_dummy;
    if (fat_find_dirent(new_raw, &lba_dummy, &off_dummy) == 0) return -1;

    u32 dir_lba, dir_off;
    if (fat_find_dirent(old_raw, &dir_lba, &dir_off) != 0) return -1;
    if (blk_read(dir_lba, 1, fat_wbuf) != 0) return -1;
    u8 *e = &fat_wbuf[dir_off];
    for (int i = 0; i < 11; i++) e[i] = (u8)new_raw[i];
    return write_sec(dir_lba, fat_wbuf);
}

/*-- Helper: find an empty dirent slot in an arbitrary directory cluster
    chain (FAT12/16 root area when parent_clus == 0).  Mirrors the body
    of fat_find_empty_dirent but parameterised on the starting cluster. */
static int find_empty_dirent_at(u32 parent_clus, u32 *dir_lba, u32 *dir_off)
{
    fat_buf_valid = 0;
    if (parent_clus == 0 && M.type != FAT32) {
        /* FAT12/16 fixed root area. */
        u32 max_sec = (M.root_ent_count * 32) / FAT_SECTOR_SIZE;
        for (u32 s = 0; s < max_sec; s++) {
            u32 lba = M.first_root_sec + s;
            if (blk_read(lba, 1, fat_wbuf) != 0) return -1;
            for (u32 e = 0; e < FAT_SECTOR_SIZE / 32; e++) {
                u8 first = fat_wbuf[e * 32];
                if (first == 0x00 || first == 0xE5) {
                    *dir_lba = lba;
                    *dir_off = e * 32;
                    return 0;
                }
            }
        }
        return 1;
    }
    /* Cluster-chain directory.  Extend the chain if every existing
       sector is full of live entries. */
    u32 c = parent_clus ? parent_clus : M.root_clus;
    while (c >= 2) {
        for (u32 s = 0; s < M.sec_per_clus; s++) {
            u32 lba = fat_clus_to_lba(c) + s;
            if (blk_read(lba, 1, fat_wbuf) != 0) return -1;
            for (u32 e = 0; e < FAT_SECTOR_SIZE / 32; e++) {
                u8 first = fat_wbuf[e * 32];
                if (first == 0x00 || first == 0xE5) {
                    *dir_lba = lba;
                    *dir_off = e * 32;
                    return 0;
                }
            }
        }
        u32 nxt = fat_next_clus(c);
        if (nxt == 0xFFFFFFFFu) {
            /* End of chain — extend with a new zeroed cluster. */
            u32 nc = fat_alloc_cluster();
            if (nc == 0) return -1;
            if (fat_set_entry(c, nc) != 0) return -1;
            for (u32 i = 0; i < FAT_SECTOR_SIZE; i++) fat_wbuf[i] = 0;
            for (u32 s = 0; s < M.sec_per_clus; s++)
                write_sec(fat_clus_to_lba(nc) + s, fat_wbuf);
            c = nc;
            continue;
        }
        c = nxt;
    }
    return 1;
}

/* Find dirent by raw 11-byte name within an arbitrary directory cluster
   chain.  parent_clus == 0 means FAT12/16 fixed root. */
static int find_dirent_at(u32 parent_clus, const char *name11,
                          u32 *dir_lba, u32 *dir_off)
{
    fat_dir_iter_t it;
    if (parent_clus == 0) {
        if (fat_diropen_root(&it) != 0) return -1;
    } else {
        if (fat_diropen_clus(&it, parent_clus) != 0) return -1;
    }
    fat_dirent_t de;
    char lfn[256];
    while (fat_dirnext(&it, &de, lfn, sizeof(lfn)) == 0) {
        if (de.attr & FAT_ATTR_VOLID) continue;
        int match = 1;
        for (int i = 0; i < 11; i++)
            if (de.name[i] != (u8)name11[i]) { match = 0; break; }
        if (match) {
            *dir_lba = it.cur_lba;
            *dir_off = (it.ent_idx - 1) * 32;
            return 0;
        }
    }
    return 1;
}

/* Resolve the parent directory of `path` and return its cluster
   number (0 for FAT12/16 root).  *leaf_out is set to the last
   component of `path` (a pointer into the original string). */
static int resolve_parent(const char *path, u32 *parent_clus_out,
                          const char **leaf_out)
{
    if (path[0] == '/') path++;
    const char *leaf = path;
    for (const char *p = path; *p; p++) if (*p == '/') leaf = p + 1;

    if (leaf == path) {
        /* Single-component path → parent is root. */
        *parent_clus_out = (M.type == FAT32) ? M.root_clus : 0;
        *leaf_out = path;
        return 0;
    }

    /* Build a NUL-terminated parent path "A/B" by copying everything
       up to (but not including) the trailing '/'. */
    char parent_path[40];
    int n = (int)(leaf - 1 - path);
    if (n >= (int)sizeof(parent_path)) return -1;
    for (int i = 0; i < n; i++) parent_path[i] = path[i];
    parent_path[n] = 0;

    fat_dir_iter_t pit;
    if (fat_diropen_path(&pit, parent_path) != 0) return -2;
    *parent_clus_out = pit.cur_clus;
    *leaf_out = leaf;
    return 0;
}

/*-- fat_mkdir: create a directory.  Accepts both single-component
    ("FOO") and nested ("A/B/C") paths.  Returns 0 on success. */
int fat_mkdir(const char *path)
{
    if (!M.valid) return -1;
    fat_buf_valid = 0;

    u32 parent_clus;
    const char *leaf;
    if (resolve_parent(path, &parent_clus, &leaf) != 0) return -2;

    char raw[11];
    build_raw_name(leaf, raw);

    /* Refuse if a same-named entry already exists in the parent. */
    u32 lba_dummy, off_dummy;
    if (find_dirent_at(parent_clus, raw, &lba_dummy, &off_dummy) == 0) return -1;

    u32 nc = fat_alloc_cluster();
    if (nc == 0) return -1;

    /* Zero the new cluster, then write . / .. into the first sector. */
    for (u32 i = 0; i < FAT_SECTOR_SIZE; i++) fat_wbuf[i] = 0;
    {
        u8 *e = &fat_wbuf[0];
        e[0] = '.';
        for (int i = 1; i < 11; i++) e[i] = ' ';
        e[11] = FAT_ATTR_DIR;
        wr16(&e[20], (u16)((nc >> 16) & 0xFFFF));
        wr16(&e[26], (u16)(nc & 0xFFFF));
        wr32(&e[28], 0);
    }
    {
        u8 *e = &fat_wbuf[32];
        e[0] = '.'; e[1] = '.';
        for (int i = 2; i < 11; i++) e[i] = ' ';
        e[11] = FAT_ATTR_DIR;
        /* ".." cluster: parent's first cluster (FAT32 root_clus or the
           parent dir's own cluster).  On FAT12/16 root, convention is
           to store 0. */
        u32 parent = parent_clus;
        wr16(&e[20], (u16)((parent >> 16) & 0xFFFF));
        wr16(&e[26], (u16)(parent & 0xFFFF));
        wr32(&e[28], 0);
    }
    u32 first_lba = fat_clus_to_lba(nc);
    if (write_sec(first_lba, fat_wbuf) != 0) {
        fat_set_entry(nc, 0); return -1;
    }
    for (u32 i = 0; i < FAT_SECTOR_SIZE; i++) fat_wbuf[i] = 0;
    for (u32 s = 1; s < M.sec_per_clus; s++) {
        if (write_sec(first_lba + s, fat_wbuf) != 0) {
            fat_set_entry(nc, 0); return -1;
        }
    }

    /* Plant the dirent in the parent. */
    u32 dir_lba, dir_off;
    if (find_empty_dirent_at(parent_clus, &dir_lba, &dir_off) != 0) {
        fat_set_entry(nc, 0); return -1;
    }
    if (blk_read(dir_lba, 1, fat_wbuf) != 0) { fat_set_entry(nc, 0); return -1; }
    u8 *e = &fat_wbuf[dir_off];
    for (int i = 0; i < 32; i++) e[i] = 0;
    for (int i = 0; i < 11; i++) e[i] = (u8)raw[i];
    e[11] = FAT_ATTR_DIR;
    wr16(&e[20], (u16)((nc >> 16) & 0xFFFF));
    wr16(&e[26], (u16)(nc & 0xFFFF));
    wr32(&e[28], 0);
    return write_sec(dir_lba, fat_wbuf);
}

/*-- fat_rmdir: remove an empty directory (accepts nested paths). -------*/
int fat_rmdir(const char *path)
{
    if (!M.valid) return -1;
    fat_buf_valid = 0;

    u32 parent_clus;
    const char *leaf;
    if (resolve_parent(path, &parent_clus, &leaf) != 0) return -2;

    char raw[11];
    build_raw_name(leaf, raw);

    u32 dir_lba, dir_off;
    if (find_dirent_at(parent_clus, raw, &dir_lba, &dir_off) != 0) return -1;
    if (blk_read(dir_lba, 1, fat_wbuf) != 0) return -1;
    u8 *e = &fat_wbuf[dir_off];
    if (!(e[11] & FAT_ATTR_DIR)) return -3;   /* not a directory */
    u32 first_clus = ((u32)rd16(&e[20]) << 16) | rd16(&e[26]);
    if (first_clus < 2) return -1;

    /* Verify directory is empty (ignoring "." and ".." entries).
       fat_dirnext walks the entire cluster chain via fat_next_clus,
       so this works for multi-cluster directories too. */
    fat_dir_iter_t it;
    if (fat_diropen_clus(&it, first_clus) != 0) return -1;
    fat_dirent_t de;
    char lfn[256];
    while (fat_dirnext(&it, &de, lfn, sizeof(lfn)) == 0) {
        if (de.name[0] == '.' && (de.name[1] == ' ' ||
            (de.name[1] == '.' && de.name[2] == ' '))) continue;
        return -4;     /* not empty */
    }

    /* Free the cluster chain and mark the dirent deleted. */
    if (fat_free_chain(first_clus) != 0) return -1;
    if (blk_read(dir_lba, 1, fat_wbuf) != 0) return -1;
    fat_wbuf[dir_off] = 0xE5;
    return write_sec(dir_lba, fat_wbuf);
}
