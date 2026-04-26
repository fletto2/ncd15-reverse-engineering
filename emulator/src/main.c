/* NCD15 emulator entry point.
 *
 * Usage: ncd15-emu [--trace] [--max-cycles N] <rom-file>
 *
 * Loads the ROM into the emulated address space at physical
 * 0x0EC00000, resets the CPU, and runs until halt. DUART
 * channel A output streams to stdout, channel B to stderr. */

#define _GNU_SOURCE
#include "emu.h"
#include "lance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <netinet/in.h>

static u32 vram_arb_read(void *ctx, u32 off, unsigned sz) {
    (void)ctx; (void)off; (void)sz; return 0xFFFFFFFFu;
}
static void vram_arb_write(void *ctx, u32 off, u32 v, unsigned sz) {
    (void)ctx; (void)off; (void)v; (void)sz;
}

/* Stub per-address access counters. Buckets are 16-byte wide so we
 * group DUART-register-sized neighbourhoods together. */
#define STUB_BUCKETS 512
static struct { u32 addr; u64 count; } stub_hot[STUB_BUCKETS];

static u32 stub_read(void *ctx, u32 off, unsigned sz) {
    (void)ctx; (void)sz;
    /* off is OFFSET-from-stub-start (0x10000000). Recover phys addr
     * and bucket it.  */
    u32 phys = 0x10000000u + off;
    u32 bucket_addr = phys & ~0xfu;
    /* Small LRU-ish bucket: linear scan, evict the least-hit slot. */
    size_t slot = 0; u64 min_count = ~(u64)0;
    for (size_t i = 0; i < STUB_BUCKETS; i++) {
        if (stub_hot[i].addr == bucket_addr) { stub_hot[i].count++; goto done; }
        if (stub_hot[i].count < min_count) { min_count = stub_hot[i].count; slot = i; }
    }
    stub_hot[slot].addr = bucket_addr;
    stub_hot[slot].count = 1;
done:
    return 0xffffffffu;
}

static void stub_dump(void) {
    fprintf(stderr, "\n--- hot unmapped-MMIO buckets (16-byte granularity) ---\n");
    for (int iter = 0; iter < 10; iter++) {
        u64 mc = 0; size_t mi = 0;
        for (size_t i = 0; i < STUB_BUCKETS; i++) {
            if (stub_hot[i].count > mc) { mc = stub_hot[i].count; mi = i; }
        }
        if (mc == 0) break;
        fprintf(stderr, "  phys 0x%08x..0x%08x  reads=%llu\n",
                stub_hot[mi].addr, stub_hot[mi].addr + 0xf,
                (unsigned long long)mc);
        stub_hot[mi].count = 0;
    }
}
static void stub_write(void *ctx, u32 off, u32 v, unsigned sz) {
    (void)ctx; (void)off; (void)v; (void)sz;
}

static void *xmalloc(size_t n) {
    void *p = calloc(1, n);
    if (!p) { perror("calloc"); exit(1); }
    return p;
}

/* --- TAP / AF_PACKET networking backend ----------------------------------
 *
 * Two modes, modelled on 3com68k's emulator:
 *   --tap <ifname>       attach to a Linux TAP device. Virtual L2 only;
 *                        needs CAP_NET_ADMIN or a pre-created tap.
 *   --raweth <ifname>    attach AF_PACKET SOCK_RAW to a real NIC. Traffic
 *                        hits the wire. Needs CAP_NET_RAW
 *                        (sudo setcap cap_net_raw+ep ./ncd15-emu).
 *
 * Both share the same fd variable (`net_fd`). LANCE TX writes to it,
 * the main loop reads from it and feeds lance_glue_recv().             */
static int net_fd = -1;
static int raw_if_index = -1;

static int tap_open(const char *ifname) {
    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("tap: open(/dev/net/tun)"); return -1; }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (ifname) strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        fprintf(stderr,
            "tap: TUNSETIFF on %s: %s\n"
            "     (need cap_net_admin, or pre-create the tap with\n"
            "      `sudo ip tuntap add dev %s mode tap user $USER`)\n",
            ifname ? ifname : "(auto)", strerror(errno),
            ifname ? ifname : "tap0");
        close(fd); return -1;
    }
    fprintf(stderr, "tap: attached to %s (fd=%d)\n", ifr.ifr_name, fd);
    return fd;
}

static int raweth_open(const char *ifname) {
    int fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK, htons(ETH_P_ALL));
    if (fd < 0) { perror("raweth: socket(AF_PACKET)"); return -1; }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "raweth: SIOCGIFINDEX(%s): %s\n", ifname, strerror(errno));
        close(fd); return -1;
    }
    raw_if_index = ifr.ifr_ifindex;
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof sll);
    sll.sll_family   = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex  = raw_if_index;
    if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) {
        fprintf(stderr, "raweth: bind(%s): %s\n", ifname, strerror(errno));
        close(fd); return -1;
    }
    /* Put the NIC in promiscuous mode so the LANCE sees every frame on
     * the wire, not just unicast-to-host + broadcast. */
    struct packet_mreq mreq;
    memset(&mreq, 0, sizeof mreq);
    mreq.mr_ifindex = raw_if_index;
    mreq.mr_type    = PACKET_MR_PROMISC;
    if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof mreq) < 0) {
        fprintf(stderr, "raweth: promisc on %s: %s (continuing)\n",
                ifname, strerror(errno));
    }
    fprintf(stderr, "raweth: attached to %s (ifindex=%d, fd=%d, promisc)\n",
            ifname, raw_if_index, fd);
    return fd;
}

static void ensure_cap_net_raw(char **argv) {
    int probe = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (probe >= 0) { close(probe); return; }
    if (errno != EPERM && errno != EACCES) return;
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0) { perror("readlink /proc/self/exe"); return; }
    exe[n] = '\0';
    fprintf(stderr,
        "raweth: CAP_NET_RAW missing — running:\n"
        "    sudo /sbin/setcap cap_net_raw+ep %s\n"
        "then re-execing self.\n", exe);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return; }
    if (pid == 0) {
        execl("/usr/bin/sudo", "sudo", "/sbin/setcap",
              "cap_net_raw+ep", exe, (char *)NULL);
        perror("execl sudo"); _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "raweth: setcap failed — continuing without.\n");
        return;
    }
    execvp(argv[0], argv);
    perror("execvp");
}

/* LANCE TX callback: frame out to the tap/raweth fd. */
static void net_send_cb(const u8 *buf, int length, void *ctx) {
    (void)ctx;
    if (net_fd < 0) return;
    if (raw_if_index >= 0) {
        struct sockaddr_ll sll;
        memset(&sll, 0, sizeof sll);
        sll.sll_family   = AF_PACKET;
        sll.sll_ifindex  = raw_if_index;
        sll.sll_protocol = htons(ETH_P_ALL);
        sll.sll_halen    = 6;
        if (length >= 6) memcpy(sll.sll_addr, buf, 6);
        ssize_t n = sendto(net_fd, buf, length, 0,
                           (struct sockaddr *)&sll, sizeof sll);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            fprintf(stderr, "raweth: sendto: %s\n", strerror(errno));
    } else {
        ssize_t n = write(net_fd, buf, length);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            fprintf(stderr, "tap: write: %s\n", strerror(errno));
    }
}

static u32 parse_ipv4(const char *s) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        fprintf(stderr, "bad IPv4: %s\n", s);
        exit(1);
    }
    return (a << 24) | (b << 16) | (c << 8) | d;
}

static u8 *load_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    size_t n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = (u8*)xmalloc(n);
    if (fread(buf, 1, n, f) != n) { perror("fread"); exit(1); }
    fclose(f);
    *out_size = n;
    return buf;
}

int main(int argc, char **argv) {
    const char *rom_path = NULL;
    const char *tap_name = NULL;
    const char *raweth_name = NULL;
    const char *nvram_path = NULL;
    const char *ip_str = NULL;
    const char *mask_str = NULL;
    const char *server_str = NULL;
    const char *gateway_str = NULL;
    const char *flash_path = NULL;
    const char *dump_shadow_path = NULL;
    int no_window = 0;
    bool trace_bus = false;
    u64 max_cycles = 0;    /* 0 == unbounded */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) trace_bus = true;
        else if (!strcmp(argv[i], "--max-cycles") && i+1 < argc) {
            max_cycles = (u64)strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--tap") && i+1 < argc) {
            tap_name = argv[++i];
        } else if (!strcmp(argv[i], "--raweth") && i+1 < argc) {
            raweth_name = argv[++i];
            ensure_cap_net_raw(argv);
        } else if (!strcmp(argv[i], "--nvram") && i+1 < argc) {
            nvram_path = argv[++i];
        } else if (!strcmp(argv[i], "--ip") && i+1 < argc) {
            ip_str = argv[++i];
        } else if (!strcmp(argv[i], "--mask") && i+1 < argc) {
            mask_str = argv[++i];
        } else if (!strcmp(argv[i], "--server") && i+1 < argc) {
            server_str = argv[++i];
        } else if (!strcmp(argv[i], "--gateway") && i+1 < argc) {
            gateway_str = argv[++i];
        } else if (!strcmp(argv[i], "--flash") && i+1 < argc) {
            flash_path = argv[++i];
        } else if (!strcmp(argv[i], "--no-window")) {
            no_window = 1;
        } else if (!strcmp(argv[i], "--dump-shadow") && i+1 < argc) {
            dump_shadow_path = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 1;
        } else {
            rom_path = argv[i];
        }
    }
    if (!rom_path) {
        fprintf(stderr, "usage: %s [--trace] [--max-cycles N] <rom.u8>\n", argv[0]);
        return 1;
    }

    size_t rom_size = 0;
    u8 *rom_bytes = load_file(rom_path, &rom_size);
    if (rom_size != NCD15_ROM_SIZE) {
        fprintf(stderr, "warning: ROM is %zu bytes, expected %u — loading as-is\n",
                rom_size, NCD15_ROM_SIZE);
    }

    /* Build bus */
    bus b; bus_init(&b, rom_bytes);
    b.trace = trace_bus;
    if (ip_str)      b.inject_ip      = parse_ipv4(ip_str);
    if (mask_str)    b.inject_mask    = parse_ipv4(mask_str);
    if (server_str)  b.inject_server  = parse_ipv4(server_str);
    if (gateway_str) b.inject_gateway = parse_ipv4(gateway_str);

    /* Linear-flash card. The boot monitor expects an "Xncd" or "Mncd"
     * magic at offset 0x10 of phys 0x1F800000 (KSEG1 0xBF800000), and
     * dispatches to the local-device boot path when shadow[0x168] == 1.
     *
     * Two acceptable formats for the flash content:
     * 1. RAW (already stripped of ECOFF wrapper): bytes start with the
     *    magic-block (b/nop/rsv/version/Xncd19r/CRC/...) at offset 0,
     *    suitable for the boot monitor's direct jalr to flash.
     * 2. ECOFF: a real Xncd15r-style file. We strip the wrapper here
     *    (file[0x140:]) AND pre-populate shadow DRAM at each section's
     *    linked paddr from the file's section data, so the X-server's
     *    own bootstrap finds its sections already in place. */
    if (flash_path) {
        size_t fsz = 0;
        u8 *fbytes = load_file(flash_path, &fsz);
        if (!fbytes) {
            fprintf(stderr, "flash: failed to load %s\n", flash_path);
            return 1;
        }
        size_t alloc_sz = 0x800000u;
        b.flash = (u8*)xmalloc(alloc_sz);
        memset(b.flash, 0xFF, alloc_sz);

        /* Detect ECOFF: f_magic == 0x0160 BE at offset 0. */
        int is_ecoff = fsz >= 0x140 && fbytes[0] == 0x01 && fbytes[1] == 0x60;
        if (is_ecoff) {
            unsigned nscns = (fbytes[2] << 8) | fbytes[3];
            /* Strip ECOFF wrapper for the flash window — flash[0]
             * needs to be the magic block, which lives at file offset
             * 0x140 (start of .text data) for Xncd-style binaries. */
            size_t flash_payload = (fsz - 0x140 < alloc_sz) ? (fsz - 0x140) : alloc_sz;
            memcpy(b.flash, fbytes + 0x140, flash_payload);
            /* Pre-load each section into shadow DRAM at its paddr.
             * Strips the high-byte (0x80→0x0E) to get the shadow offset.
             * Ranges are in [0x0EC00000, 0x0F000000) → shadow buffer. */
            int loaded = 0;
            for (unsigned i = 0; i < nscns; i++) {
                u8 *sh = fbytes + 0x4C + i*40;
                u32 paddr  = ((u32)sh[8]<<24)|((u32)sh[9]<<16)|((u32)sh[10]<<8)|sh[11];
                u32 ssize  = ((u32)sh[16]<<24)|((u32)sh[17]<<16)|((u32)sh[18]<<8)|sh[19];
                u32 scnptr = ((u32)sh[20]<<24)|((u32)sh[21]<<16)|((u32)sh[22]<<8)|sh[23];
                if (scnptr == 0 || ssize == 0) continue;  /* .bss, etc. */
                u32 phys = paddr & 0x1FFFFFFFu;  /* strip KSEG bits */
                if (phys < 0x0EC00000u || phys + ssize > 0x0F000000u) continue;
                memcpy(b.shadow + (phys - 0x0EC00000u),
                       fbytes + scnptr, ssize);
                loaded++;
                fprintf(stderr, "flash: pre-loaded section [%u] paddr=0x%08x size=0x%x → shadow+0x%x\n",
                        i, paddr, ssize, phys - 0x0EC00000u);
            }
            fprintf(stderr, "flash: ECOFF detected, %d sections pre-loaded into shadow\n", loaded);
            /* Real flash cards interleave a self-relocating bootstrap with
             * LZW-compressed sections. Our ECOFF dumps don't have that
             * format — sections are already in linked layout. Patch a
             * 2-instruction stub at flash[0] that jumps straight to the
             * linked entry. The X-server's bootstrap measures its own
             * relocation offset via `bal`/`subu`; running from the
             * linked address makes that offset zero, so it skips its
             * unpack loop and proceeds directly to bss-clear + main. */
            /* aouthdr layout: 0x14 magic/vstamp/bldrev (8B) + tsize/dsize/bsize
             * (12B) brings us to 0x24 = entry. */
            u32 entry = ((u32)fbytes[0x24]<<24)|((u32)fbytes[0x25]<<16)|
                        ((u32)fbytes[0x26]<<8) |fbytes[0x27];
            u16 hi = (entry >> 16) & 0xFFFF;
            u16 lo = entry & 0xFFFF;
            /* lui $t0, hi */
            b.flash[0] = 0x3C; b.flash[1] = 0x08;
            b.flash[2] = (u8)(hi >> 8); b.flash[3] = (u8)hi;
            /* if low half nonzero, ori $t0, $t0, lo; jr $t0 (12 B). */
            if (lo) {
                b.flash[4] = 0x35; b.flash[5] = 0x08;
                b.flash[6] = (u8)(lo >> 8); b.flash[7] = (u8)lo;
                b.flash[8] = 0x01; b.flash[9] = 0x00;
                b.flash[10] = 0x00; b.flash[11] = 0x08;
                /* delay slot nop already present at [0x0C..]. */
            } else {
                /* jr $t0 directly. */
                b.flash[4] = 0x01; b.flash[5] = 0x00;
                b.flash[6] = 0x00; b.flash[7] = 0x08;
                /* nop already at [0x08..]. */
            }
            fprintf(stderr, "flash: ECOFF entry redirect installed → 0x%08x\n", entry);
        } else {
            /* Raw flash dump — just copy verbatim. */
            memcpy(b.flash, fbytes, fsz < alloc_sz ? fsz : alloc_sz);
        }
        b.flash_size = alloc_sz;
        if (is_ecoff) {
            /* Keep file bytes for lazy re-preload on flash entry. */
            b.ecoff_bytes = fbytes;
            b.ecoff_size  = fsz;
        } else {
            free(fbytes);
        }
        fprintf(stderr, "flash: loaded %s (%zu bytes) at phys 0x1F800000 (8 MiB window)\n",
                flash_path, fsz);
    }

    /* Attach MMIO devices. Order matters if regions overlap; we want
     * the video-control/cart-ID register to shadow the first 4 bytes
     * of VRAM, so register it BEFORE the fallback VRAM mapping. */
    struct duart *d = duart_new();
    bus_add_mmio(&b, (mmio_region){
        .start  = NCD15_DUART_PHYS,  .length = 0x80,
        .read   = duart_read,  .write = duart_write,
        .ctx    = d, .name = "duart",
    });

    /* Load NVRAM image from file if given. Missing file is fine —
     * NVRAM stays at its 0xFFFF-erased default. */
    if (nvram_path) {
        if (nvram_load_file(duart_nvram(d), nvram_path) == 0)
            fprintf(stderr, "nvram: loaded from %s\n", nvram_path);
        else
            fprintf(stderr, "nvram: %s not found, starting blank\n", nvram_path);
    }

    /* VRAM access-arbiter stub at phys 0x1E200000 (KSEG1 0xBE200000).
     * The X-server polls bit 0 in tight loops before each VRAM read/write
     * — real HW sets it when the pixel-clock side is idle. We don't have
     * that conflict, so always report ready. */
    bus_add_mmio(&b, (mmio_region){
        .start = 0x1E200000u, .length = 0x10,
        .read  = vram_arb_read, .write = vram_arb_write,
        .ctx   = NULL, .name = "vram-arb",
    });

    struct vidctl *v = vidctl_new();
    bus_add_mmio(&b, (mmio_region){
        .start  = NCD15_VIDCTL_PHYS, .length = 4,
        .read   = vidctl_read, .write = vidctl_write,
        .ctx    = v, .name = "vidctl",
    });

    struct memctl *mc = memctl_new();
    bus_add_mmio(&b, (mmio_region){
        .start  = NCD15_MEMCTL_PHYS, .length = 0x100,
        .read   = memctl_read, .write = memctl_write,
        .ctx    = mc, .name = "memctl",
    });

    void *lance = lance_glue_new(&b);
    bus_add_mmio(&b, (mmio_region){
        .start  = NCD15_LANCE_PHYS, .length = 0x10,
        .read   = lance_glue_read, .write = lance_glue_write,
        .ctx    = lance, .name = "lance",
    });
    /* LANCE shared-memory window — 256 KB CPU view of the 128 KB shmem
     * with byte-lane translation. Registered AFTER the registers so
     * find_mmio finds the registers first within their narrow range. */
    extern u32  lance_shmem_mmio_read(void*, u32, unsigned);
    extern void lance_shmem_mmio_write(void*, u32, u32, unsigned);
    bus_add_mmio(&b, (mmio_region){
        .start  = NCD15_LANCE_SHMEM_PHYS, .length = 0x40000,
        .read   = lance_shmem_mmio_read, .write = lance_shmem_mmio_write,
        .ctx    = &b, .name = "lance-shmem",
    });

    /* Open the network backend if requested, and wire LANCE TX to it. */
    if (raweth_name) net_fd = raweth_open(raweth_name);
    else if (tap_name) net_fd = tap_open(tap_name);
    if (net_fd >= 0) lance_glue_set_send(lance, net_send_cb, NULL);

    struct crtc *cr = crtc_new(&b);
    bus_add_mmio(&b, (mmio_region){
        .start  = NCD15_CRTC_PHYS, .length = 0x100,
        .read   = crtc_read, .write = crtc_write,
        .ctx    = cr, .name = "crtc",
    });

    /* NVRAM 93C46 is bit-banged via the DUART's OP4/5/6 pins (OPR) and
     * read back through an IP pin — see duart.c. No separate MMIO
     * region. */

    /* Catch-all MMIO for devices we haven't emulated yet. Responds
     * with 0 on read, swallows writes. Covers everything from 0x10000000
     * up through 0x0FFFFFFF (KSEG1 = 0xB0000000..0xBFFFFFFF) — the
     * NCD15's entire I/O window except for the regions explicitly
     * claimed above. */
    bus_add_mmio(&b, (mmio_region){
        .start = 0x10000000u, .length = 0xE0000000u - 0x10000000u,
        .read = stub_read, .write = stub_write,
        .ctx = NULL, .name = "stub",
    });

    /* --- CPU --- */
    mips_cpu cpu;
    mips_reset(&cpu, &b);

    fprintf(stderr, "NCD15 emulator: ROM loaded (%zu bytes), PC=0x%08x\n",
            rom_size, cpu.pc);
    fprintf(stderr, "channel B (monitor console) -> stdout; channel A -> stderr\n");
    if (max_cycles) fprintf(stderr, "max_cycles = %llu\n", (unsigned long long)max_cycles);

    /* Non-blocking stdin so we can interleave input polling. */
    int fl = fcntl(0, F_GETFL, 0);
    fcntl(0, F_SETFL, fl | O_NONBLOCK);

    /* Run loop with periodic stdin pacing. Hold back queued bytes
     * until the DUART RX queue is empty — the monitor echoes + state-
     * machines on each char with non-trivial per-char latency, and
     * feeding a full burst causes it to silently drop later chars.
     * Buffer in our own FIFO, shuttle one byte into the DUART when
     * the monitor has consumed the previous one. */
    static u8 in_buf[4096];
    static size_t in_head = 0, in_tail = 0;
    u64 next_poll = 10000;
    u64 next_fb_refresh = 1000000;   /* refresh window every ~1M cycles */

    fb_init(!no_window);

    while (!cpu.halted && (max_cycles == 0 || cpu.cycles < max_cycles)) {
        if (fb_should_quit()) { fprintf(stderr, "fb: window closed\n"); break; }
        mips_step(&cpu);
        if (cpu.cycles >= next_fb_refresh) {
            fb_tick(&b);
            next_fb_refresh = cpu.cycles + 1000000;
            /* Drain any queued window-keyboard bytes into in_buf. */
            unsigned char kc;
            while (fb_poll_input(&kc)) {
                size_t nx = (in_tail + 1) % sizeof(in_buf);
                if (nx == in_head) break;
                in_buf[in_tail] = kc;
                in_tail = nx;
            }
        }
        if (cpu.cycles >= next_poll) {
            int delta = (int)(cpu.cycles - (next_poll - 10000));
            lance_glue_tick(lance, delta);
            lance_glue_mirror_csr0(lance);
            next_poll = cpu.cycles + 10000;
            /* Slurp from stdin into our own buffer. */
            if ((in_tail + 1) % sizeof(in_buf) != in_head) {
                u8 tmp[256];
                int n = (int)read(0, tmp, sizeof(tmp));
                for (int i = 0; i < n; i++) {
                    size_t nx = (in_tail + 1) % sizeof(in_buf);
                    if (nx == in_head) break;
                    in_buf[in_tail] = tmp[i];
                    in_tail = nx;
                }
            }
            /* Hand one byte to the DUART if it's drained. */
            if (in_head != in_tail && duart_rx_empty(d, 1)) {
                duart_feed_input(d, 1, in_buf[in_head]);
                in_head = (in_head + 1) % sizeof(in_buf);
            }
            /* RX pump: drain any pending Ethernet frames into the LANCE. */
            if (net_fd >= 0) {
                u8 eth[1600];
                for (;;) {
                    ssize_t n = read(net_fd, eth, sizeof eth);
                    if (n <= 0) break;
                    if (getenv("NCD15_TRACE_RX"))
                        fprintf(stderr, "[RX] %zd bytes dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x type=%02x%02x\n",
                                n, eth[0],eth[1],eth[2],eth[3],eth[4],eth[5],
                                eth[6],eth[7],eth[8],eth[9],eth[10],eth[11],
                                eth[12],eth[13]);
                    lance_glue_recv(lance, eth, (int)n);
                }
            }
        }
    }

    if (dump_shadow_path) {
        FILE *f = fopen(dump_shadow_path, "wb");
        if (f) {
            /* First 0x2000 bytes of shadow cover all known runtime config
             * slots (0x88, 0xC8, 0xCC, 0xEC, 0xF8..0xFD, 0x104, 0x13C,
             * 0xC38..0xCB7 NVRAM mirror). */
            fwrite(b.shadow, 1, NCD15_DRAM_SIZE, f);
            fclose(f);
            fprintf(stderr, "dump-shadow: wrote %s (0x2000 bytes)\n", dump_shadow_path);
        }
    }
    if (nvram_path) {
        if (nvram_save_file(duart_nvram(d), nvram_path) == 0)
            fprintf(stderr, "nvram: saved to %s\n", nvram_path);
        else
            fprintf(stderr, "nvram: save to %s failed\n", nvram_path);
    }

    if (cpu.halted) {
        fprintf(stderr, "\n--- CPU halted ---\n");
        mips_dump(&cpu);
    } else {
        fprintf(stderr, "\n--- exhausted max_cycles=%llu ---\n",
                (unsigned long long)cpu.cycles);
        mips_dump(&cpu);
    }

    fprintf(stderr, "\n--- MMIO access counts ---\n");
    for (size_t i = 0; i < b.nregions; i++) {
        mmio_region *r = &b.regions[i];
        if (r->read_count || r->write_count) {
            fprintf(stderr, "  %-10s [0x%08x..%08x]  R=%llu W=%llu\n",
                    r->name, r->start, r->start + r->length - 1,
                    (unsigned long long)r->read_count,
                    (unsigned long long)r->write_count);
        }
    }
    stub_dump();
    crtc_dump_hist(cr);
    return cpu.halted ? 1 : 0;
}
