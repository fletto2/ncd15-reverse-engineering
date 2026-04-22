/*
 * lance.h -- AMD Am7990 / Am79C90 LANCE Ethernet controller emulation
 *
 * License: BSD-3-Clause (inherited from MAME's am79c90 device)
 * Original (C++): Patrick Mackinlay (MAME team)
 * C port for the 3Com CS/200x emulator.
 *
 * Faithful translation of MAME src/devices/machine/am79c90.cpp + .h, with all
 * MAME-specific machinery (device_t, attotime, devcb, scheduler) replaced by
 * plain function-pointer callbacks driven by a per-cycle tick from the host
 * main loop.
 *
 * The host wires four callbacks:
 *
 *   dma_in_cb  - read a 16-bit word from host memory at the given address
 *                (used by the LANCE to fetch init blocks, descriptors, and
 *                packet data)
 *   dma_out_cb - write a 16-bit word to host memory at the given address
 *                (used by the LANCE to update descriptor ownership/status
 *                and write received packet data)
 *   intr_cb    - drive the LANCE's open-drain /INT pin. state=0 means
 *                "asserted" (low), state=1 means "deasserted" (high).
 *   send_cb    - hand a fully-formed Ethernet frame off to the host's
 *                network backend. Pass NULL to use loopback-only mode
 *                (good enough to pass the OEM POST CSR2 walk).
 *
 * The chip has 4 CSRs visible to the host CPU:
 *   CSR0 - Status / control
 *   CSR1 - Init block address bits 15:00
 *   CSR2 - Init block address bits 23:16
 *   CSR3 - Bus interface mode (BCON, ACON, BSWP)
 *
 * The host accesses them through TWO byte-wide ports on the bus:
 *   RDP - Register Data Port    (read/write the CSR currently selected by RAP)
 *   RAP - Register Address Port (selects which CSR is visible at RDP)
 *
 * The 3Com board has these wired at 0x200000 (RDP, 16-bit) and 0x200002 (RAP),
 * matching the LANCE's pinout.
 *
 * Memory model: the LANCE is a bus master. When it does DMA, it issues 16-bit
 * reads and writes against the host bus address space. We model this through
 * the dma_in_cb / dma_out_cb function pointers; the host wires them to its
 * own memory map (in our case, lance_dma_read_word / lance_dma_write_word in
 * 3com.c, which call mem_read_byte / mem_write_byte).
 */
#ifndef LANCE_H
#define LANCE_H

#include <stdint.h>

typedef struct lance_t lance_t;

typedef uint16_t (*lance_dma_in_fn)(uint32_t addr, void *ctx);
typedef void     (*lance_dma_out_fn)(uint32_t addr, uint16_t data, void *ctx);
typedef void     (*lance_intr_fn)(int state, void *ctx);
typedef void     (*lance_send_fn)(const uint8_t *buf, int length, void *ctx);

/* Variant selector (affects buffer-length encoding and a couple of details
 * in CSR2 / TMD1 handling). The 3Com board has the AM79C90 (C-LANCE). */
enum lance_variant {
    LANCE_AM7990  = 0,
    LANCE_AM79C90 = 1,
};

/* CSR0 status / control bits */
enum {
    LANCE_CSR0_INIT  = 0x0001,  /* initialize */
    LANCE_CSR0_STRT  = 0x0002,  /* start */
    LANCE_CSR0_STOP  = 0x0004,  /* stop */
    LANCE_CSR0_TDMD  = 0x0008,  /* transmit demand */
    LANCE_CSR0_TXON  = 0x0010,  /* transmitter on */
    LANCE_CSR0_RXON  = 0x0020,  /* receiver on */
    LANCE_CSR0_INEA  = 0x0040,  /* interrupt enable */
    LANCE_CSR0_INTR  = 0x0080,  /* interrupt flag */
    LANCE_CSR0_IDON  = 0x0100,  /* initialization done */
    LANCE_CSR0_TINT  = 0x0200,  /* transmit interrupt */
    LANCE_CSR0_RINT  = 0x0400,  /* receive interrupt */
    LANCE_CSR0_MERR  = 0x0800,  /* memory error */
    LANCE_CSR0_MISS  = 0x1000,  /* missed packet */
    LANCE_CSR0_CERR  = 0x2000,  /* collision error */
    LANCE_CSR0_BABL  = 0x4000,  /* babble */
    LANCE_CSR0_ERR   = 0x8000,  /* error */
};

/* MODE register bits (in the init block) */
enum {
    LANCE_MODE_DRX   = 0x0001,  /* disable receiver */
    LANCE_MODE_DTX   = 0x0002,  /* disable transmitter */
    LANCE_MODE_LOOP  = 0x0004,  /* loopback */
    LANCE_MODE_DTCR  = 0x0008,  /* disable transmit CRC */
    LANCE_MODE_COLL  = 0x0010,  /* force collision */
    LANCE_MODE_DRTY  = 0x0020,  /* disable retry */
    LANCE_MODE_INTL  = 0x0040,  /* internal loopback */
    LANCE_MODE_PROM  = 0x8000,  /* promiscuous */
};

struct lance_t {
    int variant;        /* one of enum lance_variant */

    /* Host callbacks */
    lance_dma_in_fn  dma_in_cb;
    lance_dma_out_fn dma_out_cb;
    lance_intr_fn    intr_out_cb;
    lance_send_fn    send_cb;
    void            *cb_ctx;

    /* Public CSRs */
    uint16_t rap;
    uint16_t csr[4];

    /* Init block state */
    uint16_t mode;
    uint64_t logical_addr_filter;
    uint8_t  physical_addr[6];

    uint32_t rx_ring_base;
    uint8_t  rx_ring_mask;
    uint8_t  rx_ring_pos;
    uint16_t rx_md[4];

    uint32_t tx_ring_base;
    uint8_t  tx_ring_mask;
    uint8_t  tx_ring_pos;
    uint16_t tx_md[4];

    /* /INT line state (0 = asserted/low, 1 = deasserted/high) */
    int intr_out_state;
    int idon;

    /* Internal loopback buffer (for MODE_LOOP transmit→receive) */
    uint8_t lb_buf[36];
    int     lb_length;

    /* TX poll timer (CPU cycles until next transmit_poll). Initialized
     * to LANCE_TX_POLL_CYCLES on init/reset; the host calls lance_tick()
     * to decrement it and triggers transmit_poll() when it reaches 0. */
    int tx_poll_counter;
};

/* Initialize a LANCE device. variant: LANCE_AM7990 or LANCE_AM79C90.
 * After init you must call lance_set_callbacks() before lance_regs_r/w. */
void lance_init(lance_t *l, int variant);

/* Hardware reset (equivalent to /RESET asserted). */
void lance_reset(lance_t *l);

/* Wire the host callbacks. send_cb may be NULL if you only want loopback. */
void lance_set_callbacks(lance_t *l,
                         lance_dma_in_fn dma_in,
                         lance_dma_out_fn dma_out,
                         lance_intr_fn intr,
                         lance_send_fn send,
                         void *ctx);

/* Bus accessors. The 3Com board wires:
 *   offset 0 = RDP (Register Data Port)
 *   offset 1 = RAP (Register Address Port)
 * Both are 16-bit. The host CPU reads/writes via word accesses. */
uint16_t lance_regs_r(lance_t *l, int offset);
void     lance_regs_w(lance_t *l, int offset, uint16_t data);

/* Drive the /RESET pin. state=0 means asserted (resets the chip).
 * The 3Com board ties this to the system reset line. */
void lance_reset_w(lance_t *l, int state);

/* Per-CPU-cycle tick. The host main loop calls this to drive the TX poll
 * timer and any other time-dependent state. Pass the number of CPU cycles
 * elapsed since the last call. */
void lance_tick(lance_t *l, int cycles);

/* Called by the host network backend when a packet arrives from the wire.
 * Returns 0 if the packet was accepted, negative on error. */
int lance_recv(lance_t *l, const uint8_t *buf, int length);

#endif /* LANCE_H */
