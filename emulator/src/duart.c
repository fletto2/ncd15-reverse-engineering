/* MC68681 / SCN2681 DUART — minimal emulation sufficient to run the
 * NCD15 monitor's console output.
 *
 * The NCD15 has the chip wired with a 4-byte register stride on the
 * bus, so register offset N in the datasheet sits at byte offset
 * 4*N on the CPU bus. Our bus dispatcher will feed us `offset` in
 * terms of CPU bytes; we divide by 4 to recover the DUART register
 * index before decoding.
 *
 * Scope: channel A + channel B TX path (ASCII to stdout/stderr),
 * status register TxRDY bit, minimal stubs for interrupt and
 * auxiliary registers. Input and timers are stubbed.
 *
 * Datasheet: https://www.nxp.com/docs/en/data-sheet/SCN68681.pdf */

#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUART_REG_STRIDE   4
#define DUART_NREGS        16

/* Per-channel register indices (for the MC68681 / SCN2681 view,
 * i.e. the datasheet's byte offset — NOT the NCD15's bus offset). */
enum {
    MRA_REG  = 0x00,   /* Mode register A (read/write) */
    SRA_REG  = 0x01,   /* Status A (read) / Clock-select A (write) */
    /* 0x02: reserved (read) / command A (write) */
    CRA_REG  = 0x02,
    RHRA_REG = 0x03,   /* RX holding A (read) / TX holding A (write) */
    THRA_REG = 0x03,
    /* 0x04..0x05: IPCR / ACR / ISR / IMR / CTUR / CTLR */
    MRB_REG  = 0x08,
    SRB_REG  = 0x09,
    CRB_REG  = 0x0A,
    RHRB_REG = 0x0B,
    THRB_REG = 0x0B,
};

#define SR_RXRDY   0x01
#define SR_FFULL   0x02
#define SR_TXRDY   0x04
#define SR_TXEMT   0x08

typedef struct duart {
    u8 regs[DUART_NREGS];
    u8 sra;           /* status channel A */
    u8 srb;           /* status channel B */
    u8 mr_ptr[2];     /* MR1 vs MR2 pointer per channel */
    /* A tiny 1-deep input FIFO per channel so we can feed stdin chars. */
    u8 rx_queue[2][16];
    int rx_head[2], rx_tail[2];
    bool log_tx;
} duart;

struct duart *duart_new(void) {
    duart *d = (duart*)calloc(1, sizeof(*d));
    d->sra = SR_TXRDY | SR_TXEMT;    /* ready to transmit from cold-boot */
    d->srb = SR_TXRDY | SR_TXEMT;
    d->log_tx = true;
    return d;
}

void duart_free(duart *d) { free(d); }

static void duart_tx(duart *d, int ch, u8 byte) {
    if (!d->log_tx) return;
    /* The NCD15 monitor writes its console to channel B. Put that on
     * stdout; channel A (unused by the monitor) goes to stderr so
     * anything on it isn't mistaken for program output. */
    FILE *out = (ch == 1) ? stdout : stderr;
    fputc(byte, out);
    fflush(out);
}

static int duart_rx_has(duart *d, int ch) {
    return d->rx_head[ch] != d->rx_tail[ch];
}
static u8 duart_rx_pop(duart *d, int ch) {
    if (!duart_rx_has(d, ch)) return 0;
    u8 b = d->rx_queue[ch][d->rx_head[ch]];
    d->rx_head[ch] = (d->rx_head[ch] + 1) % sizeof(d->rx_queue[0]);
    return b;
}
void duart_feed_input(duart *d, int ch, u8 byte) {
    int next = (d->rx_tail[ch] + 1) % sizeof(d->rx_queue[0]);
    if (next == d->rx_head[ch]) return;   /* drop on full */
    d->rx_queue[ch][d->rx_tail[ch]] = byte;
    d->rx_tail[ch] = next;
}

/* Is the RX queue empty for this channel? Main's stdin feeder uses
 * this to pace input so the monitor can consume each char before we
 * inject the next. */
int duart_rx_empty(struct duart *d, int ch) {
    return d->rx_head[ch] == d->rx_tail[ch];
}

u32 duart_read(void *ctx, u32 offset, unsigned size) {
    duart *d = (duart*)ctx;
    (void)size;
    unsigned reg = (offset / DUART_REG_STRIDE) & 0x1f;
    switch (reg) {
    case MRA_REG:  return d->regs[MRA_REG + d->mr_ptr[0]];
    case SRA_REG:
        /* Refresh RxRDY */
        if (duart_rx_has(d, 0)) d->sra |= SR_RXRDY; else d->sra &= ~SR_RXRDY;
        return d->sra;
    case RHRA_REG:
        return duart_rx_pop(d, 0);
    case MRB_REG:  return d->regs[MRB_REG + d->mr_ptr[1]];
    case SRB_REG:
        if (duart_rx_has(d, 1)) d->srb |= SR_RXRDY; else d->srb &= ~SR_RXRDY;
        return d->srb;
    case RHRB_REG:
        return duart_rx_pop(d, 1);
    default:
        return d->regs[reg];
    }
}

void duart_write(void *ctx, u32 offset, u32 value, unsigned size) {
    duart *d = (duart*)ctx;
    (void)size;
    unsigned reg = (offset / DUART_REG_STRIDE) & 0x1f;
    u8 v = (u8)value;
    switch (reg) {
    case MRA_REG:  /* MRA1 or MRA2 depending on pointer */
        d->regs[MRA_REG + d->mr_ptr[0]] = v;
        if (d->mr_ptr[0] == 0) d->mr_ptr[0] = 1;
        break;
    case CRA_REG:
        /* miscellaneous command — point MR back to MR1 if code 0x1 */
        if (((v >> 4) & 0x7) == 0x1) d->mr_ptr[0] = 0;
        break;
    case THRA_REG:
        duart_tx(d, 0, v);
        d->sra |= SR_TXRDY | SR_TXEMT;
        break;
    case MRB_REG:
        d->regs[MRB_REG + d->mr_ptr[1]] = v;
        if (d->mr_ptr[1] == 0) d->mr_ptr[1] = 1;
        break;
    case CRB_REG:
        if (((v >> 4) & 0x7) == 0x1) d->mr_ptr[1] = 0;
        break;
    case THRB_REG:
        duart_tx(d, 1, v);
        d->srb |= SR_TXRDY | SR_TXEMT;
        break;
    default:
        d->regs[reg] = v;
        break;
    }
}

/* --- Video control + memctl stubs (enough to let the monitor pass
 * the early init writes without exploding) --- */

typedef struct vidctl { u8 cart_id[4]; } vidctl;
struct vidctl *vidctl_new(void) {
    vidctl *v = (vidctl*)calloc(1, sizeof(*v));
    /* Return 0 for all cart-ID bytes → "no cart installed" path which
     * the monitor should treat as OK for our purposes. */
    return v;
}
u32 vidctl_read(void *ctx, u32 off, unsigned size) {
    vidctl *v = (vidctl*)ctx;
    (void)size;
    if (off < 4) return v->cart_id[off];
    return 0;
}
void vidctl_write(void *ctx, u32 off, u32 val, unsigned size) {
    (void)ctx; (void)off; (void)val; (void)size;
}

/* --- CRTC stub (0xBE380000) — enough to get past the monitor's vsync
 * polling. The monitor polls offset 0x13 for bit 8 (vsync) to
 * alternate; we toggle it every N accesses. --- */

typedef struct memctl { u32 regs[32]; } memctl;
struct memctl *memctl_new(void) { return (memctl*)calloc(1, sizeof(memctl)); }
u32  memctl_read (void *ctx, u32 off, unsigned size) {
    memctl *m = (memctl*)ctx; (void)size;
    return m->regs[(off >> 2) & 31];
}
void memctl_write(void *ctx, u32 off, u32 val, unsigned size) {
    memctl *m = (memctl*)ctx; (void)size;
    m->regs[(off >> 2) & 31] = val;
}

typedef struct crtc {
    u32 poll_count;
    u32 regs[64];
    u64 read_hist[256];
    u64 write_hist[256];
    u8  byte_store[256];    /* byte-granular register backing store */
    u32 write_vals[256];
    /* PC histogram of callers that read offset 0x00 (the hot poll). */
    u32 pc_hist[64];
    u64 pc_count[64];
    bus *b;                 /* to get caller PC */
} crtc;
struct crtc *crtc_new(bus *b) {
    crtc *c = (crtc*)calloc(1, sizeof(crtc));
    c->b = b;
    return c;
}
u32 crtc_read(void *ctx, u32 off, unsigned size) {
    crtc *c = (crtc*)ctx;
    (void)size;
    if (off < 256) c->read_hist[off]++;
    if (off == 0x00) {
        /* Log the calling PC into a small bucket table. */
        u32 pc = c->b->last_pc;
        size_t slot = 0; u64 mc = ~(u64)0;
        for (size_t i = 0; i < 64; i++) {
            if (c->pc_hist[i] == pc) { c->pc_count[i]++; goto pc_done; }
            if (c->pc_count[i] < mc) { mc = c->pc_count[i]; slot = i; }
        }
        c->pc_hist[slot] = pc;
        c->pc_count[slot] = 1;
pc_done: ;
    }
    c->poll_count++;
    /* Read from the byte-addressable backing store, using big-endian
     * byte order within a 4-byte register. This lets write/readback
     * handshakes (monitor writes 0xBB831F1D to +0x00, reads it back
     * byte-by-byte) actually match. */
    if (off + size > 256) return 0;
    u32 v = 0;
    for (unsigned i = 0; i < size; i++) v = (v << 8) | c->byte_store[off + i];
    /* Toggle bit 1 of the byte-0 status on a slow clock. The boot sync
     * loop at 0x0EC03E18 polls byte 0 for bit 1 to rise then fall. */
    if (off == 0x00 && size == 1) {
        u32 stored = v & ~(u32)0x02;
        v = stored | (((c->poll_count >> 4) & 1) ? 0x02 : 0x00);
    }
    /* Special: offset 0x13 toggles vsync (bit 8) for vsync polling. */
    if (off == 0x13 && size == 1) v = ((c->poll_count >> 4) & 1) ? 0x100 : 0;
    return v;
}
void crtc_write(void *ctx, u32 off, u32 val, unsigned size) {
    crtc *c = (crtc*)ctx;
    if (off + size > 256) return;
    if (off < 256) { c->write_hist[off]++; c->write_vals[off] = val; }
    for (unsigned i = 0; i < size; i++)
        c->byte_store[off + i] = (val >> ((size - 1 - i) * 8)) & 0xff;
    c->regs[(off >> 2) & 0x3f] = val;
}
void crtc_dump_hist(void *ctx) {
    crtc *c = (crtc*)ctx;
    fprintf(stderr, "\n--- CRTC per-offset read counts ---\n");
    for (int i = 0; i < 256; i++) {
        if (c->read_hist[i])
            fprintf(stderr, "  R +0x%02x: %llu\n", i, (unsigned long long)c->read_hist[i]);
    }
    fprintf(stderr, "\n--- CRTC per-offset write counts (and last value) ---\n");
    for (int i = 0; i < 256; i++) {
        if (c->write_hist[i])
            fprintf(stderr, "  W +0x%02x: %llu  last=0x%x\n", i,
                (unsigned long long)c->write_hist[i], c->write_vals[i]);
    }
    fprintf(stderr, "\n--- CRTC +0x00 reader PCs (top) ---\n");
    for (int iter = 0; iter < 10; iter++) {
        u64 mc = 0; int mi = -1;
        for (int i = 0; i < 64; i++) {
            if (c->pc_count[i] > mc) { mc = c->pc_count[i]; mi = i; }
        }
        if (mi < 0 || mc == 0) break;
        fprintf(stderr, "  caller pc=0x%08x  reads=%llu\n",
                c->pc_hist[mi], (unsigned long long)mc);
        c->pc_count[mi] = 0;
    }
}
