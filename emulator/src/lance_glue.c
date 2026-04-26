/* Glue between our bus and the Am79C90 LANCE engine (from
 * 3com68k/emulator, BSD-3-Clause C port of MAME).
 *
 * The NCD15 wires the LANCE at KSEG1 0xBE482000 = phys 0x0E482000.
 * The chip exposes 2 × 16-bit registers (RDP @ +0, RAP @ +2). The
 * NCD15's monitor addresses them with 16-bit word accesses, and the
 * bus uses a 16-bit stride so we translate `offset / 2` to recover
 * which of the two the CPU wants. */

#include "emu.h"
#include "lance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct lance_glue {
    lance_t chip;
    bus *b;
    void (*host_send)(const u8 *buf, int len, void *ctx);
    void *host_send_ctx;
} lance_glue;

/* LANCE DMA callbacks — 16-bit big-endian halfword in/out of host
 * memory. The LANCE is a 24-bit-address bus master. On NCD15 the
 * LANCE-side address space is the SHADOW DRAM bank (phys
 * 0x0EC00000..0x0F000000), NOT main DRAM. The monitor allocates
 * its init block + descriptor rings + packet buffers in shadow
 * (e.g. shadow VA 0x0EC3F0048) and tells LANCE about them via
 * CSR1/CSR2 with the bottom 24 bits (0x3F0048). Our DMA callback
 * adds the 0x0EC00000 base to recover the CPU phys address, then
 * goes through the bus dispatch.
 *
 * The 0xBE48_0000 MMIO window registered separately is for TR
 * controller register access, not LANCE init-block storage. */
static uint16_t lance_dma_in(uint32_t addr, void *ctx) {
    lance_glue *g = (lance_glue*)ctx;
    addr &= 0xFFFFFEu;
    uint16_t v = (uint16_t)bus_read(g->b, 0x0EC00000u + addr, 2);
    /* When trace is on and we're reading offset 0 of init block (mode word),
     * emit the JAL/JALR call stack so we can identify which monitor function
     * issued this init. The init-block address is in CSR1/CSR2 — we don't
     * easily access it here, so log every dma_in addr ending in ..48 (the
     * init-block PADR is at $...48 in the standard layout). */
    if (getenv("NCD15_TRACE_LANCE_INIT") && (addr & 0xFFF) == 0x048) {
        fprintf(stderr, "[INIT-DMA] addr=$%06x mode=$%04x stack:", addr, v);
        for (int i = 0; i < g->b->call_depth; i++)
            fprintf(stderr, " %08x", g->b->call_stack[i]);
        fprintf(stderr, "\n");
    }
    return v;
}
static void lance_dma_out(uint32_t addr, uint16_t data, void *ctx) {
    lance_glue *g = (lance_glue*)ctx;
    addr &= 0xFFFFFEu;
    bus_write(g->b, 0x0EC00000u + addr, data, 2);
}
static void lance_intr(int state, void *ctx) {
    /* We don't emulate interrupts yet — the monitor works by polling
     * the LANCE CSR0 INTR bit rather than taking interrupts, so this
     * is fine. */
    (void)state; (void)ctx;
}

/* Callback from the LANCE engine when it wants to transmit a frame.
 * Forward to the host-side network backend if wired. */
static void lance_send_frame(const uint8_t *buf, int length, void *ctx) {
    lance_glue *g = (lance_glue*)ctx;
    if (g->host_send) g->host_send(buf, length, g->host_send_ctx);
}

/* --- MMIO shim ---
 *
 * NCD15 wires the LANCE register pair within a 16-byte slot at
 * 0xBE482000-0xBE48200F. The monitor's access pattern (sub_0ec08d20 +
 * sub_0ec08cb0) reads/writes:
 *   offset 4 = RAP (Register Address Port — selects which CSR is
 *              visible on RDP; lance.c index 1)
 *   offset 6 = RDP (Register Data Port; lance.c index 0)
 * INVERTED from what 3com68k uses (where RDP comes first). The
 * mapping below picks the right index based on bit 1 of the offset
 * being clear (RAP) vs set (RDP). Other offsets in the slot (e.g.
 * 0xE) are unmapped — return 0 / drop. */

/* The LANCE on this board has read and write ports at DIFFERENT
 * offsets within the 0xBE482000 slot — same byte-lane convention as
 * the LANCE shmem and DUART (write at 4/6, read at +2 from the
 * write address):
 *     offset 4 = WRITE RAP  (CSR-select)
 *     offset 6 = WRITE RDP  (CSR-data)  AND byte read of RDP
 *     offset 8 = halfword read of RDP   (sub_0ec17498 OR's offset+2)
 */
static int lance_write_offset_to_reg(u32 offset) {
    switch (offset & 0xE) {
    case 4: return 1;   /* RAP */
    case 6: return 0;   /* RDP */
    default: return -1;
    }
}
static int lance_read_offset_to_reg(u32 offset) {
    switch (offset & 0xE) {
    case 4: return 1;   /* RAP read mirror */
    case 6: return 0;   /* RDP read (byte) */
    case 8: return 0;   /* RDP read (halfword) */
    case 0xa: return 1; /* RAP read mirror */
    default: return -1;
    }
}

static u32 lance_mmio_read(void *ctx, u32 offset, unsigned size) {
    lance_glue *g = (lance_glue*)ctx;
    int reg = lance_read_offset_to_reg(offset);
    (void)size;
    if (reg < 0) return 0;
    return lance_regs_r(&g->chip, reg);
}

static void lance_mmio_write(void *ctx, u32 offset, u32 value, unsigned size) {
    lance_glue *g = (lance_glue*)ctx;
    int reg = lance_write_offset_to_reg(offset);
    (void)size;
    if (reg < 0) return;
    lance_regs_w(&g->chip, reg, (uint16_t)value);
}

void *lance_glue_new(bus *b) {
    lance_glue *g = (lance_glue*)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->b = b;
    lance_init(&g->chip, LANCE_AM79C90);
    lance_set_callbacks(&g->chip, lance_dma_in, lance_dma_out,
                        lance_intr, lance_send_frame, g);
    lance_reset(&g->chip);
    return g;
}

void lance_glue_set_send(void *glue,
                         void (*send)(const u8 *buf, int len, void *ctx),
                         void *ctx) {
    lance_glue *g = (lance_glue*)glue;
    g->host_send = send;
    g->host_send_ctx = ctx;
}

/* Compute Ethernet CRC-32 (same poly as the LANCE chip's). Used to
 * append a valid FCS to incoming frames before passing them to the
 * chip — AF_PACKET strips the FCS, but lance_receive verifies it. */
static uint32_t eth_crc32(const u8 *data, int n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}
void lance_glue_recv(void *glue, const u8 *buf, int len) {
    lance_glue *g = (lance_glue*)glue;
    /* Snoop ARP replies and update the host's ARP-cache slot at
     * data_0x0EC000FE (boot server MAC). The real chip's IRQ handler
     * walks the RX descriptor on each completion and parses the frame
     * to update the cache; we don't run interrupts, so do the parse
     * here. Keeps the rest of the boot path (which depends on this MAC
     * to assemble the TFTP UDP frame) able to proceed.
     *
     * ARP reply format: dst-mac(6) src-mac(6) type=0806(2) hw=0001(2)
     * proto=0800(2) hwlen=6 plen=4 op=0002(2) sender-mac(6)
     * sender-ip(4) target-mac(6) target-ip(4). Total 42 bytes (60 with
     * Ethernet-min padding). */
    if (len >= 42 && buf[12] == 0x08 && buf[13] == 0x06 &&
        buf[20] == 0x00 && buf[21] == 0x02 /* op = reply */) {
        const u8 *sender_mac = &buf[22];
        const u8 *sender_ip  = &buf[28];
        u32 sip = ((u32)sender_ip[0] << 24) | ((u32)sender_ip[1] << 16) |
                  ((u32)sender_ip[2] <<  8) |  sender_ip[3];
        if (g->b->inject_server && sip == g->b->inject_server) {
            memcpy(&g->b->shadow[0xFE], sender_mac, 6);
            /* Set bit 1 of the 32-bit BE word at runtime addr 0x0EC009A0
             * (= shadow[0x9A0..0x9A3]). The IP-search loop in
             * sub_0ec11774_dump (line 0x0EC11C34) reads `lw 0x918($s1)`
             * with $s1=0x0EC00088 and exits when bit 1 is set. OR'ing
             * the low byte preserves whatever else is in the word. */
            g->b->shadow[0x9A3] |= 0x02;
            fprintf(stderr, "[ARP cache] boot-server %u.%u.%u.%u → "
                    "%02x:%02x:%02x:%02x:%02x:%02x\n",
                    sender_ip[0], sender_ip[1], sender_ip[2], sender_ip[3],
                    sender_mac[0], sender_mac[1], sender_mac[2],
                    sender_mac[3], sender_mac[4], sender_mac[5]);
        }
    }
    /* AF_PACKET delivers frames without FCS. Append a valid FCS so the
     * chip's CRC check passes. Cap at our internal buffer; min Ethernet
     * frame is 60 bytes (without FCS), and we have headroom up to 1518. */
    static u8 framed[1600];
    if (len < 0 || len + 4 > (int)sizeof(framed)) {
        lance_recv(&g->chip, buf, len);
        return;
    }
    memcpy(framed, buf, len);
    uint32_t crc = eth_crc32(buf, len);
    framed[len + 0] = crc & 0xff;
    framed[len + 1] = (crc >> 8) & 0xff;
    framed[len + 2] = (crc >> 16) & 0xff;
    framed[len + 3] = (crc >> 24) & 0xff;
    lance_recv(&g->chip, framed, len + 4);
}

/* Mirror the chip's CSR0 into the host's state struct at shadow[0x8F8].
 * The boot POST's loopback test (sub_0ec17f4c) polls this struct
 * waiting for IDON to appear there — but on real HW the update goes
 * through an IRQ handler that reads CSR0 and writes the mirror.
 * Without an IRQ delivery path on our emulator, we update the mirror
 * directly here. */
void lance_glue_mirror_csr0(void *glue) {
    lance_glue *g = (lance_glue*)glue;
    u16 csr0 = g->chip.csr[0];
    /* int_cnt reset on STOP must run BEFORE the dormant early-return below,
     * otherwise int_cnt accumulates across phases (each new TX cycle starts
     * with STOP→INIT→STRT, and we'd never reset). State scoped to this
     * function via static — kept here so it fires regardless of whether
     * we mirror this tick. */
    static u16 prev_csr0_outer;
    static u8  int_cnt_outer;
    if (csr0 & LANCE_CSR0_STOP) int_cnt_outer = 0;
    /* Only mirror once the chip has been told to do something (init,
     * start, txon, rxon all clear → chip is dormant, don't trample
     * shadow[0x8F8] which the early memtest also writes). */
    if (csr0 == LANCE_CSR0_STOP || csr0 == 0) {
        prev_csr0_outer = csr0;
        return;
    }
    if (!(csr0 & (LANCE_CSR0_IDON | LANCE_CSR0_INIT |
                  LANCE_CSR0_STRT | LANCE_CSR0_TXON | LANCE_CSR0_RXON))) return;
    g->b->shadow[0x8F8] = (csr0 >> 8) & 0xff;
    g->b->shadow[0x8F9] = csr0 & 0xff;
    /* Second CSR0 mirror at data_0x0EC00870 — sub_0ec17504 spins on this
     * waiting for TINT/RINT/etc. */
    g->b->shadow[0x870] = (csr0 >> 8) & 0xff;
    g->b->shadow[0x871] = csr0 & 0xff;
    /* Loopback self-test (sub_0ec17f4c) gates also poll two scalar state
     * mirrors that are normally written by the IRQ handler we don't run:
     *   data_0x0EC0072A halfword: bit 2 = "init complete"
     *   data_0x0EC0148E halfword: == 1 means "LANCE up"
     * Force them once IDON has been observed. */
    if (csr0 & LANCE_CSR0_IDON) {
        /* data_0x0EC0072A bit 2 — "init complete". Test at 0x0EC1812C. */
        u16 v = ((u16)g->b->shadow[0x72A] << 8) | g->b->shadow[0x72B];
        v |= 0x4;
        g->b->shadow[0x72A] = (v >> 8) & 0xff;
        g->b->shadow[0x72B] = v & 0xff;
    }
    /* IRQ-counter mirror at runtime address 0x0EC0148E (= $s1+0x1406 with
     * $s1=0x0EC00088; the dis annotates as data_0x0EC01406 which is wrong).
     * The IRQ handler increments this on each interrupt (IDON / TINT / RINT)
     * and the boot's POST checks expect:
     *   init phase    (sub_0ec18120): == 1 (one IDON since STOP)
     *   transmit test (sub_0ec18334): == 2 (one IDON + one TINT since STOP)
     * Reset on STOP, increment on each newly-asserted INT-causing bit. */
    u16 newly_set = csr0 & ~prev_csr0_outer;
    if (newly_set & LANCE_CSR0_IDON) int_cnt_outer++;
    if (newly_set & LANCE_CSR0_TINT) int_cnt_outer++;
    u8 int_cnt = int_cnt_outer;
    prev_csr0_outer = csr0;
    /* RINT deliberately omitted: on real HW the test's spin loop
     * (sub_0ec17504) exits on TINT alone and reads int_cnt before RINT
     * fires. In our emulator both TINT and RINT are asserted in one
     * lance_transmit_poll call, so counting RINT would push int_cnt to
     * 3 and fail the == 2 check at 0x0EC18338. */
    g->b->shadow[0x148E] = 0;
    g->b->shadow[0x148F] = int_cnt;
}

void lance_glue_tick(void *glue, int cycles) {
    lance_glue *g = (lance_glue*)glue;
    lance_tick(&g->chip, cycles);
}

u32 lance_glue_read(void *ctx, u32 off, unsigned sz) {
    return lance_mmio_read(ctx, off, sz);
}
void lance_glue_write(void *ctx, u32 off, u32 v, unsigned sz) {
    lance_mmio_write(ctx, off, v, sz);
}
