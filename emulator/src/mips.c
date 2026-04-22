/* MIPS-I R3052 interpreter for the NCD15 emulator.
 *
 * Scope: enough instructions for the NCD15 boot monitor to run. No
 * MMU, no TLB, no FPU. Implements branch-delay-slot semantics by
 * using two-PC-step (pc, next_pc). */

#include "emu.h"
#include <stdio.h>
#include <stdlib.h>

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

static u32 fetch(mips_cpu *cpu, u32 va) {
    return bus_read(cpu->bus, va, 4);
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
        case 12: cpu->cp0_status   = v; break;
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

void mips_step(mips_cpu *cpu) {
    if (cpu->halted) return;
    u32 pc   = cpu->pc;
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
    case 0x28: { u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn)); bus_write(cpu->bus, a, cpu->r[RT(insn)] & 0xff,   1); break; } /* SB */
    case 0x29: { u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn)); bus_write(cpu->bus, a, cpu->r[RT(insn)] & 0xffff, 2); break; } /* SH */
    case 0x2B: { u32 a = cpu->r[RS(insn)] + sign_ext16(IMM(insn)); bus_write(cpu->bus, a, cpu->r[RT(insn)],          4); break; } /* SW */
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
        cpu->next_pc = cpu->branch_target;
    } else {
        cpu->next_pc = cpu->pc + 4;
    }
    cpu->cycles++;
}

void mips_run(mips_cpu *cpu, u64 max_cycles) {
    /* Simple hot-PC sampler: every STRIDE cycles, bump a counter for
     * the current PC. Dump the top-N on completion. */
    enum { SAMPLE_STRIDE = 1024, HOT_SLOTS = 32 };
    u32 hot_pc[HOT_SLOTS] = {0};
    u64 hot_count[HOT_SLOTS] = {0};

    while (!cpu->halted && (max_cycles == 0 || cpu->cycles < max_cycles)) {
        mips_step(cpu);
        if ((cpu->cycles & (SAMPLE_STRIDE - 1)) == 0) {
            u32 p = cpu->pc;
            int slot = -1, victim = 0;
            u64 min_count = ~(u64)0;
            for (int i = 0; i < HOT_SLOTS; i++) {
                if (hot_pc[i] == p) { slot = i; break; }
                if (hot_count[i] < min_count) { min_count = hot_count[i]; victim = i; }
            }
            if (slot < 0) { hot_pc[victim] = p; hot_count[victim] = 1; }
            else hot_count[slot]++;
        }
    }

    fprintf(stderr, "\n--- hot PCs (sample every %d cycles) ---\n", SAMPLE_STRIDE);
    for (int iter = 0; iter < 10; iter++) {
        u64 maxc = 0; int mi = -1;
        for (int i = 0; i < HOT_SLOTS; i++) {
            if (hot_count[i] > maxc) { maxc = hot_count[i]; mi = i; }
        }
        if (mi < 0 || maxc == 0) break;
        fprintf(stderr, "  pc=0x%08x  samples=%llu\n",
                hot_pc[mi], (unsigned long long)maxc);
        hot_count[mi] = 0;
    }
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
