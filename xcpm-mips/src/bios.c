/*============================================================================
 * bios.c -- XCP/M-68K BIOS (§7)
 *
 * 23 standard CP/M-68K BIOS functions. Console functions are real (call
 * the UART driver). Disk functions (§7 entries 8-14, 16) are stubs that
 * return success without doing anything; the FAT32 layer above the BDOS
 * is what actually moves data, so the BIOS disk surface is vestigial in
 * XCP/M-68K (§13/§18b commentary).
 *==========================================================================*/
#include "xcpm.h"
#include "bios.h"

/* Forward decl */
extern void default_exc_handler(void);

/*-- Per-call kernel scratch (RAM, BSS) -------------------------------------*/
static u8  iobyte;
static u16 cur_drive, cur_track, cur_sector;
static u32 cur_dma;

/*-- Memory region table (BIOS 18) -----------------------------------------*/
struct memrgn {
    u16 nregions;
    u32 start;
    u32 length;
} __attribute__((packed));

static struct memrgn memtab = {
    1, RAM_TPA_ADDR, (RAM_END_ADDR - RAM_TPA_ADDR)
};

/*-- Synthetic DPH (just a placeholder so seldsk has something to return) --*/
static u8 dirbuf[128];
static struct {
    u32 xlt;        /* 0 = identity */
    u16 scratch[3];
    u16 reserved;
    u32 dirbuf;
    u32 dpb;
    u32 csv;
    u32 alv;
} dph0;

static struct {
    u16 spt;
    u8  bsh, blm;
    u8  exm, fill;
    u16 dsm;
    u16 drm;
    u8  al0, al1;
    u16 cks;
    u16 off;
} dpb0 = {
    256,        /* spt */
    4, 15,      /* bsh, blm  -> 2 KB blocks */
    0, 0,
    8175,       /* dsm */
    4095,       /* drm */
    0, 0,
    0,
    0
};

/*-- 23 BIOS handlers ------------------------------------------------------*/

static u32 b_init(u32 a, u32 b)         { (void)a;(void)b; return 0; }   /* default drive A */

static u32 b_wboot(u32 a, u32 b)        { (void)a;(void)b;
    /* TODO: re-enter CCP */
    return 0;
}

extern int bdos_redir_getc(void);
extern int bdos_redir_status(void);

static u32 b_constat(u32 a, u32 b)      { (void)a;(void)b;
    int rs = bdos_redir_status();
    if (rs >= 0) return (u32)rs;
    return (uart_poll() >= 0) ? 0xFF : 0;
}

static u32 b_conin(u32 a, u32 b)        { (void)a;(void)b;
    int rc = bdos_redir_getc();
    if (rc >= 0) return (u32)(rc & 0xFF);
    return (u32)(uart_getc() & 0xFF);
}

extern int bdos_redir_putc(int c);     /* if redirect active, consumes byte */
static u32 b_conout(u32 c, u32 b)       { (void)b;
    int byte = (int)(c & 0xFF);
    if (bdos_redir_putc(byte)) return 0;
    uart_putc(byte);
    return 0;
}

static u32 b_lstout(u32 c, u32 b)       { (void)c;(void)b; return 0; }
static u32 b_auxout(u32 c, u32 b)       { (void)c;(void)b; return 0; }
static u32 b_auxin (u32 a, u32 b)       { (void)a;(void)b; return 0x1A; }   /* EOF */
static u32 b_home  (u32 a, u32 b)       { (void)a;(void)b; cur_track = 0; return 0; }

static u32 b_seldsk(u32 d, u32 b)       { (void)b;
    if ((d & 0xF) != 0) return 0;       /* only A: for now */
    cur_drive = (u16)d;
    return (u32)&dph0;
}

static u32 b_settrk(u32 t, u32 b)       { (void)b; cur_track  = (u16)t; return 0; }
static u32 b_setsec(u32 s, u32 b)       { (void)b; cur_sector = (u16)s; return 0; }
static u32 b_setdma(u32 a, u32 b)       { (void)b; cur_dma = a; return 0; }

/* Disk read/write are no-ops in XCP/M-68K (FAT32 layer bypasses) */
static u32 b_read (u32 a, u32 b)        { (void)a;(void)b; return 1; } /* error */
static u32 b_write(u32 m, u32 b)        { (void)m;(void)b; return 1; } /* error */

static u32 b_lststat   (u32 a, u32 b)   { (void)a;(void)b; return 0xFF; }
static u32 b_sectran   (u32 s, u32 xlt) { (void)xlt; return s; }   /* identity */
static u32 b_conoutstat(u32 a, u32 b)   { (void)a;(void)b; return 0xFF; }
static u32 b_getseg    (u32 a, u32 b)   { (void)a;(void)b; return (u32)&memtab; }
static u32 b_getiob    (u32 a, u32 b)   { (void)a;(void)b; return iobyte; }
static u32 b_setiob    (u32 v, u32 b)   { (void)b; iobyte = (u8)v; return 0; }
static u32 b_flush     (u32 a, u32 b)   { (void)a;(void)b; return 0; }

static u32 b_setexc(u32 vec, u32 addr)
{
    if (vec >= 256) return 0;
#if defined(__mips__)
    /* No 68K RAM vector table on MIPS; exceptions vector to 0x80000080.
       SETEXC is a no-op here for now. */
    (void)addr;
    return 0;
#else
    u32 old = RAM_VEC_TABLE[vec];
    RAM_VEC_TABLE[vec] = addr ? addr : (u32)default_exc_handler;
    return old;
#endif
}

static u32 b_auxinstat (u32 a, u32 b)   { (void)a;(void)b; return 0; }
static u32 b_auxoutstat(u32 a, u32 b)   { (void)a;(void)b; return 0xFF; }

/*-- Dispatch table --------------------------------------------------------*/
typedef u32 (*bios_handler_t)(u32, u32);

static bios_handler_t bios_table[BIOS_NFUNCS] = {
    [BIOS_INIT      ] = b_init,
    [BIOS_WBOOT     ] = b_wboot,
    [BIOS_CONSTAT   ] = b_constat,
    [BIOS_CONIN     ] = b_conin,
    [BIOS_CONOUT    ] = b_conout,
    [BIOS_LSTOUT    ] = b_lstout,
    [BIOS_AUXOUT    ] = b_auxout,
    [BIOS_AUXIN     ] = b_auxin,
    [BIOS_HOME      ] = b_home,
    [BIOS_SELDSK    ] = b_seldsk,
    [BIOS_SETTRK    ] = b_settrk,
    [BIOS_SETSEC    ] = b_setsec,
    [BIOS_SETDMA    ] = b_setdma,
    [BIOS_READ      ] = b_read,
    [BIOS_WRITE     ] = b_write,
    [BIOS_LSTSTAT   ] = b_lststat,
    [BIOS_SECTRAN   ] = b_sectran,
    [BIOS_CONOUTSTAT] = b_conoutstat,
    [BIOS_GETSEG    ] = b_getseg,
    [BIOS_GETIOB    ] = b_getiob,
    [BIOS_SETIOB    ] = b_setiob,
    [BIOS_FLUSH     ] = b_flush,
    [BIOS_SETEXC    ] = b_setexc,
    [BIOS_AUXINSTAT ] = b_auxinstat,
    [BIOS_AUXOUTSTAT] = b_auxoutstat,
};

u32 bios_dispatch(u32 func, u32 p1, u32 p2)
{
    if (func >= BIOS_NFUNCS) return 0;
    bios_handler_t h = bios_table[func];
    if (!h) return 0;
    return h(p1, p2);
}

u32 bios_call(u32 func, u32 p1, u32 p2)
{
    return bios_dispatch(func, p1, p2);
}

void bios_init(void)
{
    /* Wire DPH0 → DPB0 → dirbuf */
    dph0.dirbuf = (u32)dirbuf;
    dph0.dpb    = (u32)&dpb0;

#if !defined(__mips__)
    /* 68K: hook BIOS on TRAP #3 (vector 35). MIPS reaches BIOS through the
       BDOS syscall path / direct calls — no 68K vector table. */
    RAM_VEC_TABLE[35] = (u32)bios_trap_entry;
#endif
}
