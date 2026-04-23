/* MIPS-I R3052 interpreter for the NCD15 emulator.
 *
 * Scope: enough instructions for the NCD15 boot monitor to run. No
 * MMU, no TLB, no FPU. Implements branch-delay-slot semantics by
 * using two-PC-step (pc, next_pc). */

#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 sign_ext16(u16 v) { return (i32)(i16)v; }
static u32 sign_ext8 (u8  v) { return (i32)(i8)v; }

/* Trace: set MIPS_TRACE=1 in env to log every instruction. */
static int mips_trace = -1;

static void maybe_init_trace(void) {
    if (mips_trace < 0) {
        const char *s = getenv("MIPS_TRACE");
        mips_trace = (s && *s && *s != '0') ? 1 : 0;
    }
}

void mips_reset(mips_cpu *cpu, bus *b) {
    for (int i = 0; i < 32; i++) cpu->r[i] = 0;
    cpu->hi = cpu->lo = 0;
    cpu->pc = 0xBFC00000u;      /* reset vector */
    cpu->next_pc = cpu->pc + 4;
    cpu->cp0_status = 0x00400000u;   /* BEV set, all else zero (monitor's init val) */
    cpu->cp0_cause  = 0;
    cpu->cp0_epc    = 0;
    cpu->cp0_prid   = 0x00000230u;   /* R3000A/R3052 family, arbitrary revision */
    cpu->in_delay_slot = false;
    cpu->branch_taken  = false;
    cpu->bus    = b;
    cpu->cycles = 0;
    cpu->halted = false;
    icache_flush_all(&cpu->icache);
    maybe_init_trace();
}

static inline u32 rd_reg(mips_cpu *cpu, unsigned i) { return cpu->r[i]; }
static inline void wr_reg(mips_cpu *cpu, unsigned i, u32 v) {
    if (i != 0) cpu->r[i] = v;
}

static void do_branch(mips_cpu *cpu, u32 target) {
    cpu->branch_taken  = true;
    cpu->branch_target = target;
}

/* --- I-cache implementation --- */

void icache_flush_all(icache *c) {
    for (int i = 0; i < ICACHE_SETS; i++) c->sets[i].tag = 0xFFFFFFFFu;
}

void icache_invalidate(icache *c, u32 pa) {
    u32 index = (pa >> 4) & (ICACHE_SETS - 1);
    u32 tag   = pa >> (4 + 8);       /* everything above the index bits */
    if (c->sets[index].tag == tag) c->sets[index].tag = 0xFFFFFFFFu;
}

/* Fetch a 32-bit instruction from VA. Uses I-cache for KSEG0 / KUSEG;
 * bypasses it for KSEG1 (uncached by hardware decree). */
static u32 fetch(mips_cpu *cpu, u32 va) {
    bool uncached = (va >= 0xA0000000u && va < 0xC0000000u);   /* KSEG1 */
    if (uncached) {
        return bus_read(cpu->bus, va, 4);
    }
    u32 pa = mips_va_to_pa(va);
    u32 index = (pa >> 4) & (ICACHE_SETS - 1);
    u32 tag   = pa >> (4 + 8);
    u32 word  = (pa >> 2) & (ICACHE_WORDS_PER_LINE - 1);
    icache_line *ln = &cpu->icache.sets[index];
    if (ln->tag != tag) {
        /* Miss: refill the line from the bus. */
        u32 line_base_va = va & ~(u32)(ICACHE_LINE_BYTES - 1);
        for (int i = 0; i < ICACHE_WORDS_PER_LINE; i++) {
            ln->words[i] = bus_read(cpu->bus, line_base_va + i*4, 4);
        }
        ln->tag = tag;
        cpu->icache.misses++;
    } else {
        cpu->icache.hits++;
    }
    return ln->words[word];
}

/* Instruction decode helpers. */
#define OP(x)      (((x) >> 26) & 0x3f)
#define RS(x)      (((x) >> 21) & 0x1f)
#define RT(x)      (((x) >> 16) & 0x1f)
#define RD(x)      (((x) >> 11) & 0x1f)
#define SHAMT(x)   (((x) >>  6) & 0x1f)
#define FUNCT(x)   ( (x)        & 0x3f)
#define IMM(x)     ( (x)        & 0xffff)
#define TARGET(x)  ( (x)        & 0x03ffffff)

static void unhandled(mips_cpu *cpu, u32 pc, u32 insn, const char *tag) {
    fprintf(stderr, "mips: UNHANDLED %s at PC=0x%08x insn=0x%08x op=%02x funct=%02x\n",
            tag, pc, insn, OP(insn), FUNCT(insn));
    cpu->halted = true;
}

static void exec_special(mips_cpu *cpu, u32 pc, u32 insn) {
    unsigned rs = RS(insn), rt = RT(insn), rd = RD(insn);
    unsigned sh = SHAMT(insn);
    u32 s = cpu->r[rs], t = cpu->r[rt];

    switch (FUNCT(insn)) {
    case 0x00: wr_reg(cpu, rd, t << sh); break;                       /* SLL */
    case 0x02: wr_reg(cpu, rd, t >> sh); break;                       /* SRL */
    case 0x03: wr_reg(cpu, rd, (u32)((i32)t >> sh)); break;           /* SRA */
    case 0x04: wr_reg(cpu, rd, t << (s & 31)); break;                 /* SLLV */
    case 0x06: wr_reg(cpu, rd, t >> (s & 31)); break;                 /* SRLV */
    case 0x07: wr_reg(cpu, rd, (u32)((i32)t >> (s & 31))); break;     /* SRAV */
    case 0x08: do_branch(cpu, s); break;                              /* JR */
    case 0x09: wr_reg(cpu, rd, pc + 8); do_branch(cpu, s); break;     /* JALR */
    case 0x0C: fprintf(stderr, "syscall at 0x%08x\n", pc); cpu->halted = true; break;
    case 0x0D: fprintf(stderr, "break at 0x%08x code=%x\n", pc, (insn>>6)&0xfffff); cpu->halted = true; break;
    case 0x10: wr_reg(cpu, rd, cpu->hi); break;                       /* MFHI */
    case 0x11: cpu->hi = s; break;                                    /* MTHI */
    case 0x12: wr_reg(cpu, rd, cpu->lo); break;                       /* MFLO */
    case 0x13: cpu->lo = s; break;                                    /* MTLO */
    case 0x18: { /* MULT */
        i64 p = (i64)(i32)s * (i64)(i32)t;
        cpu->lo = (u32)p; cpu->hi = (u32)(p >> 32); break;
    }
    case 0x19: { /* MULTU */
        u64 p = (u64)s * (u64)t;
        cpu->lo = (u32)p; cpu->hi = (u32)(p >> 32); break;
    }
    case 0x1A: { /* DIV */
        if (t == 0) { cpu->hi = s; cpu->lo = (i32)s < 0 ? 1 : 0xffffffffu; break; }
        cpu->lo = (u32)((i32)s / (i32)t);
        cpu->hi = (u32)((i32)s % (i32)t);
        break;
    }
    case 0x1B: { /* DIVU */
        if (t == 0) { cpu->hi = s; cpu->lo = 0xffffffffu; break; }
        cpu->lo = s / t; cpu->hi = s % t; break;
    }
    case 0x20: /* ADD — trap on overflow; we just add */
    case 0x21: wr_reg(cpu, rd, s + t); break;                         /* ADDU */
    case 0x22: /* SUB */
    case 0x23: wr_reg(cpu, rd, s - t); break;                         /* SUBU */
    case 0x24: wr_reg(cpu, rd, s & t); break;                         /* AND */
    case 0x25: wr_reg(cpu, rd, s | t); break;                         /* OR */
    case 0x26: wr_reg(cpu, rd, s ^ t); break;                         /* XOR */
    case 0x27: wr_reg(cpu, rd, ~(s | t)); break;                      /* NOR */
    case 0x2A: wr_reg(cpu, rd, (i32)s < (i32)t ? 1 : 0); break;       /* SLT */
    case 0x2B: wr_reg(cpu, rd, s < t ? 1 : 0); break;                 /* SLTU */
    default: unhandled(cpu, pc, insn, "SPECIAL"); break;
    }
}

static void exec_regimm(mips_cpu *cpu, u32 pc, u32 insn) {
    unsigned rs = RS(insn), rt = RT(insn);
    i32 s = (i32)cpu->r[rs];
    u32 target = (pc + 4) + (sign_ext16(IMM(insn)) << 2);

    switch (rt) {
    case 0x00: if (s <  0) do_branch(cpu, target); break;                 /* BLTZ */
    case 0x01: if (s >= 0) do_branch(cpu, target); break;                 /* BGEZ */
    case 0x10: wr_reg(cpu, 31, pc + 8); if (s <  0) do_branch(cpu, target); break; /* BLTZAL */
    case 0x11: wr_reg(cpu, 31, pc + 8); if (s >= 0) do_branch(cpu, target); break; /* BGEZAL */
    default: unhandled(cpu, pc, insn, "REGIMM"); break;
    }
}

static void exec_cop0(mips_cpu *cpu, u32 pc, u32 insn) {
    unsigned rs = RS(insn), rt = RT(insn), rd = RD(insn);
    if (rs == 0x00) {            /* MFC0 */
        u32 v = 0;
        switch (rd) {
        case 8:  v = cpu->cp0_badvaddr; break;
        case 12: v = cpu->cp0_status;   break;
        case 13: v = cpu->cp0_cause;    break;
        case 14: v = cpu->cp0_epc;      break;
        case 15: v = cpu->cp0_prid;     break;
        default: break;
        }
        wr_reg(cpu, rt, v);
    } else if (rs == 0x04) {     /* MTC0 */
        u32 v = cpu->r[rt];
        switch (rd) {
        case 8:  cpu->cp0_badvaddr = v; break;
        case 12:
            /* R3052 Status bit 16 = IsC (isolate cache). When the
             * cache-init code sets IsC, it writes through cached
             * addresses to clear tags; subsequent stores hit only the
             * cache (not memory). We just flush the whole I-cache
             * here to keep our model consistent. */
            if ((v & 0x10000u) && !(cpu->cp0_status & 0x10000u))
                icache_flush_all(&cpu->icache);
            cpu->cp0_status = v;
            break;
        case 13: cpu->cp0_cause    = v; break;
        case 14: cpu->cp0_epc      = v; break;
        default: break;   /* silently ignore */
        }
    } else if (rs == 0x10) {     /* CO funct — RFE, TLBR, etc. */
        switch (FUNCT(insn)) {
        case 0x10:  /* RFE — restore status stack */
            cpu->cp0_status = (cpu->cp0_status & ~0xfu) | ((cpu->cp0_status >> 2) & 0xfu);
            break;
        default: /* ignore */ break;
        }
    } else {
        unhandled(cpu, pc, insn, "COP0");
    }
}

/* --- Interrupt / exception delivery --- */

/* Fire an exception, vector to 0x80000080 (or 0xBFC00180 if BEV=1).
 * For plain interrupts, ExcCode=0. */
static void take_exception(mips_cpu *cpu, u32 pc, u32 exccode) {
    bool in_ds = cpu->in_delay_slot;
    cpu->cp0_epc = in_ds ? (pc - 4) : pc;
    /* Shift KU/IE stack left by 2 (3 levels: current→prev, prev→old). */
    u32 ki = cpu->cp0_status & 0x3f;
    cpu->cp0_status = (cpu->cp0_status & ~0x3fu) | ((ki << 2) & 0x3cu);
    cpu->cp0_cause &= ~0x8000007fu;
    cpu->cp0_cause |= (exccode & 0x1f) << 2;
    if (in_ds) cpu->cp0_cause |= 0x80000000u;
    cpu->pc = (cpu->cp0_status & 0x00400000u) ? 0xBFC00180u : 0x80000080u;
    cpu->next_pc = cpu->pc + 4;
    cpu->branch_taken = false;
    cpu->in_delay_slot = false;
}

/* Interrupt check: fires when Cause.IP & Status.IM is non-zero and
 * IEc is set. The monitor's tick counter (data_0x0EC006A8) has no
 * writer in the disassembly — its incrementer lives in a handler at
 * VA 0x80000080 that the monitor installs at boot. We don't synthesize
 * a timer here because there's nothing installed at 0x80000080 in
 * DRAM yet by the time the first EXC would fire; an exception would
 * fall through NOPs for 4 MiB before hitting real code and thrash
 * 100M+ cycles each go. If/when we need real interrupts (for DUART
 * RX, LANCE RX, etc.) we'll raise IP from the respective device
 * model, not on a periodic schedule. */
static void check_interrupts(mips_cpu *cpu, u32 pc) {
    if (!(cpu->cp0_status & 1)) return;            /* IEc cleared */
    u32 pending = (cpu->cp0_cause  >> 8) & 0xff;
    u32 mask    = (cpu->cp0_status >> 8) & 0xff;
    if (pending & mask)
        take_exception(cpu, pc, 0);
}

void mips_step(mips_cpu *cpu) {
    if (cpu->halted) return;
    check_interrupts(cpu, cpu->pc);
    u32 pc   = cpu->pc;
    /* Trap: log every time PC lands on the BFC00140 or BFC00180
     * exception vector, plus the prior PC that branched here. */
    static u32 prev_pc = 0;
    static int trap_count = 0;
    if ((pc == 0x1fc007cc || pc == 0x1fc00140 || pc == 0x1fc00180)
        && trap_count < 8) {
        fprintf(stderr, "TRAP: pc=0x%08x entered from prev=0x%08x  ra=0x%08x  cycles=%llu\n",
                pc, prev_pc, cpu->r[31], (unsigned long long)cpu->cycles);
        trap_count++;
    }
    prev_pc = pc;
    cpu->bus->last_pc = pc;
    cpu->bus->cur_cycles = cpu->cycles;
    /* Additionally, update last_pc right before each LW path below so
     * the CRTC read attribution is on the actual LW, not the fetched
     * instruction's address. Set once here for fetch; loads re-set
     * before they call bus_read. */
    u32 insn = fetch(cpu, pc);
    if (mips_trace) {
        fprintf(stderr, "%08x: %08x\n", pc, insn);
    }

    /* Two-PC delay-slot semantics.
     *
     *   cpu->pc      = address of THIS instruction (being executed)
     *   cpu->next_pc = address of the instruction AFTER this one
     *                  — normally pc+4; during a branch's delay slot
     *                  it holds the branch target.
     *
     * A branch instruction sets branch_taken + branch_target here. At
     * the end of this function we:
     *   pc := next_pc              (advance to delay slot / next insn)
     *   next_pc := branch_target   (if a branch just fired)
     *           or pc + 4          (normal straight-line advance)
     */
    cpu->branch_taken = false;

    switch (OP(insn)) {
    case 0x00: exec_special(cpu, pc, insn); break;
    case 0x01: exec_regimm (cpu, pc, insn); break;
    case 0x02: {                                                       /* J */
        cpu->branch_taken = true;
        cpu->branch_target = ((pc + 4) & 0xf0000000u) | (TARGET(insn) << 2);
        break;
    }
    case 0x03: {                                                       /* JAL */
        wr_reg(cpu, 31, pc + 8);
        cpu->branch_taken = true;
        cpu->branch_target = ((pc + 4) & 0xf0000000u) | (TARGET(insn) << 2);
        break;
    }
    case 0x04: if (cpu->r[RS(insn)] == cpu->r[RT(insn)])               /* BEQ */
                   do_branch(cpu, (pc + 4) + (sign_ext16(IMM(insn)) << 2));
               break;
    case 0x05: if (cpu->r[RS(insn)] != cpu->r[RT(insn)])               /* BNE */
                   do_branch(cpu, (pc + 4) + (sign_ext16(IMM(insn)) << 2));
               break;
    case 0x06: if ((i32)cpu->r[RS(insn)] <= 0)                          /* BLEZ */
                   do_branch(cpu, (pc + 4) + (sign_ext16(IMM(insn)) << 2));
               break;
    case 0x07: if ((i32)cpu->r[RS(insn)] >  0)                          /* BGTZ */
                   do_branch(cpu, (pc + 4) + (sign_ext16(IMM(insn)) << 2));
               break;
    case 0x08: /* ADDI */
    case 0x09: wr_reg(cpu, RT(insn), cpu->r[RS(insn)] + sign_ext16(IMM(insn))); break; /* ADDIU */
    case 0x0A: wr_reg(cpu, RT(insn), (i32)cpu->r[RS(insn)] < (i32)sign_ext16(IMM(insn)) ? 1 : 0); break; /* SLTI */
    case 0x0B: wr_reg(cpu, RT(insn), cpu->r[RS(insn)] < sign_ext16(IMM(insn)) ? 1 : 0); break;           /* SLTIU */
    case 0x0C: wr_reg(cpu, RT(insn), cpu->r[RS(insn)] & IMM(insn)); break;           /* ANDI */
    case 0x0D: wr_reg(cpu, RT(insn), cpu->r[RS(insn)] | IMM(insn)); break;           /* ORI */
    case 0x0E: wr_reg(cpu, RT(insn), cpu->r[RS(insn)] ^ IMM(insn)); break;           /* XORI */
    case 0x0F: wr_reg(cpu, RT(insn), (u32)IMM(insn) << 16); break;                   /* LUI */
    case 0x10: exec_cop0(cpu, pc, insn); break;
    /* loads */
    case 0x20: { u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn)); wr_reg(cpu, RT(insn), sign_ext8 ((u8 )bus_read(cpu->bus, a, 1))); break; } /* LB */
    case 0x21: { u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn)); wr_reg(cpu, RT(insn), (u32)(i32)(i16)bus_read(cpu->bus, a, 2)); break; } /* LH */
    case 0x23: { u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn)); wr_reg(cpu, RT(insn), bus_read(cpu->bus, a, 4)); break; } /* LW */
    case 0x24: { u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn)); wr_reg(cpu, RT(insn), bus_read(cpu->bus, a, 1)); break; } /* LBU */
    case 0x25: { u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn)); wr_reg(cpu, RT(insn), bus_read(cpu->bus, a, 2)); break; } /* LHU */
    /* stores */
    case 0x28: { u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn)); bus_write(cpu->bus, a, cpu->r[RT(insn)] & 0xff,   1);
        if (!(a >= 0xA0000000u && a < 0xC0000000u)) icache_invalidate(&cpu->icache, mips_va_to_pa(a));
        break; } /* SB */
    case 0x29: { u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn)); bus_write(cpu->bus, a, cpu->r[RT(insn)] & 0xffff, 2);
        if (!(a >= 0xA0000000u && a < 0xC0000000u)) icache_invalidate(&cpu->icache, mips_va_to_pa(a));
        break; } /* SH */
    case 0x2B: { u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn)); bus_write(cpu->bus, a, cpu->r[RT(insn)],          4);
        if (!(a >= 0xA0000000u && a < 0xC0000000u)) icache_invalidate(&cpu->icache, mips_va_to_pa(a));
        break; } /* SW */
    /* Unaligned LWL/LWR/SWL/SWR — GCC emits these for unaligned
     * accesses. Monitor uses them for string copies. */
    case 0x22: { /* LWL */
        u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn));
        u32 aligned = a & ~3u;
        unsigned shift = (a & 3) * 8;
        u32 word = bus_read(cpu->bus, aligned, 4);
        u32 mask = (shift == 0) ? 0u : ((1u << shift) - 1);
        /* Place the high bytes of the unaligned word into the top of rt */
        wr_reg(cpu, RT(insn), (cpu->r[RT(insn)] & mask) | (word << (24 - shift)));
        break;
    }
    case 0x26: { /* LWR */
        u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn));
        u32 aligned = a & ~3u;
        unsigned shift = (3 - (a & 3)) * 8;
        u32 word = bus_read(cpu->bus, aligned, 4);
        u32 mask = (shift == 0) ? 0u : (~0u << (32 - shift));
        wr_reg(cpu, RT(insn), (cpu->r[RT(insn)] & mask) | (word >> shift));
        break;
    }
    case 0x2A: { /* SWL */
        u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn));
        u32 aligned = a & ~3u;
        unsigned shift = (a & 3) * 8;
        u32 mem = bus_read(cpu->bus, aligned, 4);
        u32 mask = (shift == 0) ? 0u : ((1u << shift) - 1);
        u32 v = (mem & ~mask) | (cpu->r[RT(insn)] >> (24 - shift));
        bus_write(cpu->bus, aligned, v, 4);
        break;
    }
    case 0x2E: { /* SWR */
        u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn));
        u32 aligned = a & ~3u;
        unsigned shift = (3 - (a & 3)) * 8;
        u32 mem = bus_read(cpu->bus, aligned, 4);
        u32 mask = (shift == 0) ? 0u : (~0u << (32 - shift));
        u32 v = (mem & mask) | (cpu->r[RT(insn)] << shift);
        bus_write(cpu->bus, aligned, v, 4);
        break;
    }
    default: unhandled(cpu, pc, insn, "OP"); break;
    }

    /* Advance pc to next_pc (delay slot or next linear instruction).
     * Then compute the new next_pc: the branch target if a branch
     * just fired in THIS instruction, else pc+4 (linear). */
    cpu->pc = cpu->next_pc;
    if (cpu->branch_taken) {
        /* Optional debug: log segment-crossing branches so we can see
         * when execution moves between the ROM (0xBFCxxxx / 0x1FCxxxx),
         * KSEG0 (0x8xxx), and DRAM-linked monitor VA (0x0EC xxxx). */
        static int log_branches = -1;
        if (log_branches < 0) {
            const char *s = getenv("MIPS_BRANCH_LOG");
            log_branches = (s && *s && *s != '0') ? 1 : 0;
        }
        if (log_branches) {
            u32 src_top = pc         & 0xf0000000u;
            u32 dst_top = cpu->branch_target & 0xf0000000u;
            if (src_top != dst_top) {
                fprintf(stderr, "branch: 0x%08x -> 0x%08x  (insn=0x%08x)\n",
                        pc, cpu->branch_target, insn);
            }
        }
        cpu->next_pc = cpu->branch_target;
    } else {
        cpu->next_pc = cpu->pc + 4;
    }
    cpu->cycles++;
}

void mips_run(mips_cpu *cpu, u64 max_cycles) {
    /* PC histogram bucketed by 4-byte cells for the phase we care about.
     * Reset at cycle RESET_AT so we get a clean picture of the post-
     * DRAM-copy main-monitor phase, not diluted by the 50M-cycle reset
     * phase. Covers a 128 KB window around the main monitor VA range. */
    enum { RESET_AT = 40000000ULL, SAMPLE_STRIDE = 256 };
    /* Sample PCs in two windows: main-monitor DRAM (0x0EC00000+) AND
     * the cached ROM alias at 0x1FC00000+. Buckets are 4-byte cells. */
    #define MAIN_BASE 0x0EC00000u
    #define MAIN_SIZE 0x00040000u
    #define ROM_BASE  0x1FC00000u
    #define ROM_WSIZE 0x00001000u
    #define WIN_CELLS ((MAIN_SIZE + ROM_WSIZE) / 4)
    #define WIN_BASE 0   /* unused with new layout */
    static u32 hist[WIN_CELLS];
    bool reset_done = false;

    while (!cpu->halted && (max_cycles == 0 || cpu->cycles < max_cycles)) {
        mips_step(cpu);
        if (!reset_done && cpu->cycles >= RESET_AT) {
            memset(hist, 0, sizeof(hist));
            reset_done = true;
        }
        if ((cpu->cycles & (SAMPLE_STRIDE - 1)) == 0) {
            u32 p = cpu->pc;
            if (p >= MAIN_BASE && p < MAIN_BASE + MAIN_SIZE)
                hist[(p - MAIN_BASE) / 4]++;
            else if (p >= ROM_BASE && p < ROM_BASE + ROM_WSIZE)
                hist[(MAIN_SIZE + (p - ROM_BASE)) / 4]++;
        }
    }

    fprintf(stderr, "\n--- hot PCs (sample/%d after cycle %llu) ---\n",
            SAMPLE_STRIDE, (unsigned long long)RESET_AT);
    for (int iter = 0; iter < 20; iter++) {
        u32 maxc = 0, mi = 0;
        for (u32 i = 0; i < WIN_CELLS; i++) {
            if (hist[i] > maxc) { maxc = hist[i]; mi = i; }
        }
        if (maxc == 0) break;
        u32 addr = (mi * 4 < MAIN_SIZE) ? (MAIN_BASE + mi*4)
                                         : (ROM_BASE + (mi*4 - MAIN_SIZE));
        fprintf(stderr, "  pc=0x%08x  samples=%u\n", addr, maxc);
        hist[mi] = 0;
    }
    #undef MAIN_BASE
    #undef MAIN_SIZE
    #undef ROM_BASE
    #undef ROM_WSIZE
    #undef WIN_BASE
    #undef WIN_CELLS
}

void mips_dump(const mips_cpu *cpu) {
    fprintf(stderr, "=== MIPS state @ cycle %llu ===\n", (unsigned long long)cpu->cycles);
    fprintf(stderr, " pc = %08x   next = %08x\n", cpu->pc, cpu->next_pc);
    for (int i = 0; i < 32; i += 4) {
        fprintf(stderr, "  r%02d=%08x  r%02d=%08x  r%02d=%08x  r%02d=%08x\n",
                i, cpu->r[i], i+1, cpu->r[i+1], i+2, cpu->r[i+2], i+3, cpu->r[i+3]);
    }
    fprintf(stderr, " hi=%08x lo=%08x  status=%08x cause=%08x epc=%08x\n",
            cpu->hi, cpu->lo, cpu->cp0_status, cpu->cp0_cause, cpu->cp0_epc);
}
