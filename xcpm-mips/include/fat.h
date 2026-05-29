/*============================================================================
 * fat.h -- FAT12/16/32 read-only driver (§11)
 *==========================================================================*/
#ifndef XCPM_FAT_H
#define XCPM_FAT_H

#include "xcpm.h"

#define FAT_MAX_OPEN    16
#define FAT_SECTOR_SIZE 512

typedef enum { FAT_NONE = 0, FAT12 = 12, FAT16 = 16, FAT32 = 32 } fat_type_t;

/* Mount metadata derived from the BPB. */
typedef struct {
    u8         valid;
    fat_type_t type;
    u32        bytes_per_sec;
    u32        sec_per_clus;
    u32        rsvd_sec;
    u32        num_fats;
    u32        root_ent_count;        /* FAT12/16; 0 on FAT32 */
    u32        total_sec;
    u32        fat_sec;               /* sectors per FAT */
    u32        root_clus;             /* FAT32 only */
    u32        first_data_sec;
    u32        first_root_sec;        /* FAT12/16 only; LBA of root dir */
    u32        cluster_count;
} fat_mount_t;

/* Per-open-file handle. */
typedef struct {
    u8  in_use;
    u32 first_clus;
    u32 cur_clus;
    u32 cur_pos;            /* byte position within file */
    u32 file_size;
    u32 dir_lba;            /* dirent location: sector ... */
    u32 dir_off;            /* ... and offset within sector */
    u8  attr;
} fat_file_t;

/* 8.3 directory entry as it appears on disk. */
typedef struct __attribute__((packed)) {
    u8  name[11];
    u8  attr;
    u8  ntres;
    u8  ctime_tenth;
    u16 ctime, cdate;
    u16 adate;
    u16 first_clus_hi;
    u16 wtime, wdate;
    u16 first_clus_lo;
    u32 file_size;
} fat_dirent_t;

#define FAT_ATTR_RO       0x01
#define FAT_ATTR_HIDDEN   0x02
#define FAT_ATTR_SYS      0x04
#define FAT_ATTR_VOLID    0x08
#define FAT_ATTR_DIR      0x10
#define FAT_ATTR_ARCHIVE  0x20
#define FAT_ATTR_LFN      0x0F

/* High-level API */
int  fat_mount(void);
const fat_mount_t *fat_get_mount(void);

/* Directory iteration.  Embeds a 512-byte sector buffer and a 256-byte
 * LFN reassembly buffer for a total of 786 bytes per iter.  Stack-
 * allocating an iter inside the kernel can starve the supervisor
 * stack on tight-RAM targets — see CLAUDE.md "Kernel stack vs. CCP
 * BSS".  Future work: shrink this; a previous attempt regressed boot. */
typedef struct {
    u32  cur_clus;
    u32  cur_lba;
    u32  ent_idx;       /* dirent index within current sector */
    u8   buf[FAT_SECTOR_SIZE];
    u8   buf_lba_valid;
    u32  buf_lba;
    char lfn[256];
    u32  lfn_len;
} fat_dir_iter_t;

int  fat_diropen_root  (fat_dir_iter_t *it);
int  fat_diropen_clus  (fat_dir_iter_t *it, u32 clus);
int  fat_diropen_path  (fat_dir_iter_t *it, const char *path);
int  fat_dirnext       (fat_dir_iter_t *it, fat_dirent_t *out, char *lfn_out, int lfn_max);

/* File I/O */
int  fat_fopen (const char *path, fat_file_t *fp);     /* path = "/foo.txt" */
int  fat_fread (fat_file_t *fp, void *buf, u32 max);   /* returns bytes read */
int  fat_fseek (fat_file_t *fp, u32 byte_pos);          /* absolute seek */
void fat_fclose(fat_file_t *fp);
u32  fat_fsize (fat_file_t *fp);

/* Write path */
int  fat_fcreate(const char *path, fat_file_t *fp);
int  fat_fwrite (fat_file_t *fp, const void *buf, u32 n);
int  fat_funlink(const char *path);
int  fat_frename(const char *old_path, const char *new_path);
int  fat_mkdir  (const char *path);
int  fat_rmdir  (const char *path);
u32  fat_count_free_clusters(void);
int  fat_fflush (fat_file_t *fp);   /* update dirent size on disk */

/* Match CP/M-style 8.3 pattern with ? wildcards.  Returns 1 on match. */
int  fat_match_83(const u8 *raw11, const char *pattern);

/* Search-by-pattern iterator -- like fat_dirnext but only returns dirents
   whose 8.3 name matches an 11-char wildcard pattern (spaces pad both
   base and ext, ? = match any). */
int  fat_search_first(fat_dir_iter_t *it, const char *pattern11,
                      fat_dirent_t *out);
int  fat_search_next (fat_dir_iter_t *it, const char *pattern11,
                      fat_dirent_t *out);

/* Walk the FAT chain (read one entry). */
u32  fat_next_clus(u32 clus);

/* Convert a cluster number to LBA of its first sector. */
u32  fat_clus_to_lba(u32 clus);

#endif
