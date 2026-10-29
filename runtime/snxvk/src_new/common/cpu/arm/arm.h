/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ARM_H
#define SNX_ARM_H

#include <stddef.h>
#include <stdint.h>

/* Default: Cortex-A57, ARMv8-A AArch32 user mode, little-endian guest memory.
   Separate from the A64 module ABI: never cast CPU* to SnxArmCPU*. */
#define SNX_ARM_ABI 4 /* 2: shared S/D/Q register bank and FPSCR; 3: the backend's rt; 4: the bus hook */
#define SNX_ARM_T   (1u << 5)
#define SNX_ARM_E   (1u << 9)

typedef enum {
    SNX_ARM_OK,
    SNX_ARM_UNDEFINED,
    SNX_ARM_THUMB16_UNSUPPORTED,
    SNX_ARM_THUMB32_UNSUPPORTED,
    SNX_ARM_SVC,
    SNX_ARM_BREAKPOINT,
    SNX_ARM_DATA_ABORT,
    SNX_ARM_ALIGNMENT,
    SNX_ARM_BAD_ENTRY,
    SNX_ARM_ENDIAN_UNSUPPORTED,
    SNX_ARM_BACKEND_UNSUPPORTED,
    SNX_ARM_PREFETCH_ABORT,
    SNX_ARM_CODE_CHANGED,
    SNX_ARM_IT_ENTRY /* a Thumb block entered with ITSTATE in CPSR, inside an IT block: the syndrome is the ITSTATE */
} SnxArmExit;

struct SnxArmRt;
typedef struct SnxArmCPU {
    uint32_t r[16];          /* r15 is the current instruction address, not the pipelined PC */
    uint32_t cpsr;           /* initialize to USR (0x10); NZCV, Q, GE and T are preserved */
    uint32_t exit, syndrome; /* SVC/BKPT immediate, fault address, or unsupported word */
    uint8_t* mem;
    uint64_t mem_size; /* flat mapping, at most 2^32 bytes; no MMIO */
    /* An optional bus hook for the regions whose bus rules a plain access cannot follow: mirrors
       finer than a page, byte write widths. Reads return the value with size_write = the width;
       writes pass size_write = 0x100 | width and value. Like the fetch callback, none is needed by
       default: without one every access goes straight to mem. */
    uint32_t (*io)(struct SnxArmCPU* cpu, uint32_t address, uint32_t size_write, uint32_t value);
    uint64_t exclusive_value;
    uint32_t exclusive_address, exclusive_size, exclusive_valid;
    uint32_t tpidrurw, tpidruro; /* AArch32 user TLS registers, initialized by the runtime */
    uint32_t fpscr;              /* NZCV, QC, AHP, DN, FZ, RMode and cumulative exceptions */
    uint64_t d[32];              /* S(2n) = Dn[31:0], S(2n+1) = Dn[63:32], Qn = D(2n+1):D(2n) */
    const struct SnxArmRt* rt;   /* the backend (the runtime running the code), or NULL: see SnxArmRt */
} SnxArmCPU;

/* What the backend decides for code the blocks can't run on their own. Generated code calls the runtime library
   (snx_arm_undefined and snx_arm_coproc, rt_helpers.c), which calls the backend's function when cpu->rt has it, and
   otherwise does what is written by each. A function leaves r15 alone: the block sets it */
typedef struct SnxArmRt {
    /* An instruction the block compiler doesn't implement, or can't with the block's memory model. word is the
       instruction as fetched (a Thumb32 one's halfwords as hw1 | hw2 << 16), reason the exit the block would stop
       with (SNX_ARM_UNDEFINED, SNX_ARM_BACKEND_UNSUPPORTED). The backend runs it and returns with exit
       SNX_ARM_OK, and the block goes on after it; or it sets exit (and syndrome), and the block stops at it.
       Without one: exit = reason, syndrome = word */
    void (*undefined)(SnxArmCPU* cpu, uint32_t word, uint32_t reason);
    /* MRC, MCR, MRRC or MCRR to coprocessor 14 or 15, as its A32 encoding (a Thumb one's A32 twin, condition AL):
       the backend reads or writes the registers it names, or sets exit to SNX_ARM_UNDEFINED (syndrome = word) for
       one user mode can't reach. Without one: snx_arm_coproc_user */
    void (*coproc)(SnxArmCPU* cpu, uint32_t word);
} SnxArmRt;

/* the runtime library's side (rt_helpers.c, in snx_arm_rt and every 32-bit game module) */
void snx_arm_undefined(SnxArmCPU* cpu, uint32_t word, uint32_t reason);
void snx_arm_coproc(SnxArmCPU* cpu, uint32_t word);
/* the coprocessor registers the architecture gives user mode with no system to ask: TPIDRURW (read and write) and
   TPIDRURO (read) from the cpu's fields; any other is SNX_ARM_UNDEFINED. A backend can end with it */
void snx_arm_coproc_user(SnxArmCPU* cpu, uint32_t word);

typedef void (*SnxArmBlock)(SnxArmCPU* cpu);

typedef enum {
    SNX_A32_UNDEFINED,
    SNX_A32_DATA,
    SNX_A32_SINGLE,
    SNX_A32_EXTRA,
    SNX_A32_MULTIPLE,
    SNX_A32_BRANCH,
    SNX_A32_BX,
    SNX_A32_BLX_IMM,
    SNX_A32_MOVW,
    SNX_A32_MOVT,
    SNX_A32_MUL,
    SNX_A32_MULL,
    SNX_A32_MLS,
    SNX_A32_DIV,
    SNX_A32_CLZ,
    SNX_A32_REV,
    SNX_A32_EXTEND,
    SNX_A32_BITFIELD,
    SNX_A32_MRS,
    SNX_A32_MSR,
    SNX_A32_EXCLUSIVE,
    SNX_A32_ACQUIRE_RELEASE,
    SNX_A32_CLREX,
    SNX_A32_BARRIER,
    SNX_A32_HINT,
    SNX_A32_SVC,
    SNX_A32_BKPT,
    SNX_A32_TLS,
    SNX_A32_VECTOR,
    SNX_A32_UADD8,
    SNX_A32_SEL,
    SNX_A32_MULH, /* ARMv5TE signed halfword multiplies (ARM11 profile) */
    SNX_A32_COPROC, /* MRC/MCR/MRRC/MCRR to coprocessor 14 or 15: the backend's (SnxArmRt.coproc) */
    SNX_A32_SATURATE,   /* SSAT, USAT, SSAT16, USAT16 */
    SNX_A32_PACK,       /* PKHBT, PKHTB */
    SNX_A32_SELECT,     /* SEL */
    SNX_A32_PARALLEL,   /* the parallel add and subtract family: {S,Q,SH,U,UQ,UH}{ADD16,ASX,SAX,SUB16,ADD8,SUB8} */
    SNX_A32_QARITH,     /* QADD, QSUB, QDADD, QDSUB */
    SNX_A32_HALF_MUL,   /* SMLA<x><y>, SMUL<x><y>, SMLAW<y>, SMULW<y>, SMLAL<x><y> */
    SNX_A32_DUAL_MUL,   /* SMLAD, SMUAD, SMLSD, SMUSD, SMLALD, SMLSLD, SMMLA, SMMUL, SMMLS */
    SNX_A32_ABS_DIFF,   /* USAD8, USADA8 */
    SNX_A32_CRC32,      /* CRC32 and CRC32C, byte, halfword and word */
    SNX_A32_SETEND      /* SETEND: E, which a block reads only on entry, so the block ends after it */
} SnxA32Kind;

/* Reserved, privileged and unpredictable encodings are rejected.
   Decode is independent of LLVM and does not execute an instruction. */
SnxA32Kind snx_a32_decode(uint32_t word);

/* Opt-in ISA profiles. Existing decode/emission APIs remain Cortex-A57.
   ARM11 is an integer ARMv6K user-mode subset, not a full ARM11 implementation.
   A9 (the Vita's Cortex-A9 MPCore, ARMv7-A) is A57's, with the UNPREDICTABLE forms its games rely on executed as the
   A9 executes them (profile.c).
   ARM7TDMI is the AGB board's CPU on flat memory: misaligned loads rotate and stores align, stores of the PC write
   PC + 12, STM writes the written back base unless it comes first, Thumb's POP {pc} keeps the state, LDM and STM
   take an unaligned base, and SnxArmCPU.io sees the board's I/O regions. It decodes as A57, but for an MSR that
   writes the mode, which is UNDEFINED for the board's runtime to run */
typedef enum { SNX_ARM_PROFILE_A57, SNX_ARM_PROFILE_ARM11, SNX_ARM_PROFILE_A9, SNX_ARM_PROFILE_ARM7TDMI } SnxArmProfile;
SnxA32Kind snx_a32_decode_profile(SnxArmProfile profile, uint32_t word);

/* ---- Thumb16/Thumb32 ----
   Decoded to an explicit operand model (unlike A32, whose emitter re-decodes
   its word): Thumb16 uses 3-bit register fields and Thumb32 spreads operands
   over two halfwords, so normalising up front keeps the emitter simple. */
typedef enum {
    TH_UNDEF,
    TH_SHIFT_IMM, /* LSL/LSR/ASR Rd, Rm, #imm */
    TH_ADDSUB,    /* ADD/SUB Rd, Rn, Rm|#imm3; 3-register and immediate */
    TH_IMM8,      /* MOV/CMP/ADD/SUB Rd, #imm8 */
    TH_ALU,       /* AND..MVN and RSB; op in .op */
    TH_HI,        /* ADD/CMP/MOV high registers; BX/BLX register */
    TH_LOAD,      /* all load forms: .flags carries size/sign/addressing */
    TH_STORE,     /* all store forms */
    TH_ADDR,      /* ADR, ADD Rd, SP, ADD/SUB SP */
    TH_EXTEND,    /* SXTB/SXTH/UXTB/UXTH */
    TH_PUSHPOP,   /* PUSH/POP */
    TH_LDMSTM,    /* LDMIA/STMIA */
    TH_BRANCH,    /* B/Bcc/BL/BLX/BX/CBZ/CBNZ; .op selects */
    TH_MUL,       /* MUL/MLA/MLS */
    TH_BITFIELD,  /* SBFX/UBFX/BFI/BFC */
    TH_WIMM,      /* MOVW/MOVT/ADDW/SUBW */
    TH_WDATA,     /* wide data-processing (shifted register) */
    TH_TABLE,     /* TBB/TBH */
    TH_IT,        /* If-Then */
    TH_HINT,      /* NOP/YIELD/WFE/WFI/SEV */
    TH_LEGACY_BL, /* ARM11 and ARM7TDMI: separate Thumb BL prefix / BL or (ARM11) BLX suffix */
    TH_REV,       /* ARM11 Thumb REV/REV16/REVSH */
    TH_A32,       /* a Thumb32 instruction that is an A32 one in another encoding: .imm is the A32 word (condition
                     AL) and .op its SnxA32Kind, emitted by the A32 emitter with Thumb's PC. .ra is a doubleword
                     exclusive's second register + 1 (A32's is the first + 1), else 0, and .rd the offset T1's LDREX
                     and STREX add to the base (A32's have none). Coprocessor, VFP and Advanced
                     SIMD, LDM/STM, exclusives, barriers, MRS/MSR, long multiplies and divides, REV and CLZ */
    TH_DUAL       /* LDRD/STRD: .op 1 to load, .rd and .rm the pair, .imm the offset, P/U/W in .flags */
} SnxThumbShape;

/* ALU sub-operations for TH_ALU / TH_WDATA / TH_ADDSUB. */
enum {
    ALU_AND,
    ALU_EOR,
    ALU_LSL,
    ALU_LSR,
    ALU_ASR,
    ALU_ADC,
    ALU_SBC,
    ALU_ROR,
    ALU_TST,
    ALU_NEG,
    ALU_CMP,
    ALU_CMN,
    ALU_ORR,
    ALU_MUL,
    ALU_BIC,
    ALU_MVN,
    ALU_ADD,
    ALU_SUB,
    ALU_RSB,
    ALU_ORN
};
/* Branch sub-operations. */
enum { BR_B, BR_BCOND, BR_BL, BR_BLX_IMM, BR_BX, BR_BLX_REG, BR_CBZ, BR_CBNZ, BR_BW, BR_SVC, BR_BKPT, BR_BCOND_W };
/* TH_IMM8 sub-operations. */
enum { I8_MOV, I8_CMP, I8_ADD, I8_SUB };

/* Addressing/store flags for TH_LOAD/TH_STORE. */
#define TH_B      (1u << 0)  /* byte */
#define TH_H      (1u << 1)  /* halfword */
#define TH_SIGN   (1u << 2)  /* sign-extended load */
#define TH_SPB    (1u << 3)  /* base is SP */
#define TH_PCB    (1u << 4)  /* base is PC (literal) */
#define TH_PRE    (1u << 5)  /* pre-indexed (else post-indexed) */
#define TH_UP     (1u << 6)  /* add offset (else subtract) */
#define TH_WB     (1u << 7)  /* write back */
#define TH_S      (1u << 8)  /* set flags */
#define TH_REG    (1u << 9)  /* register offset (else immediate) */
#define TH_RSHIFT (1u << 10) /* TH_WDATA operand shifted by a register */
#define TH_MODIMM (1u << 11) /* TH_WDATA operand is a modified immediate in .imm */
#define TH_ROTC   (1u << 12) /* ... a rotated one: ThumbExpandImm_C's carry is its bit 31 */

#define TH_F16 1u /* instruction length in halfwords */
#define TH_F32 2u
typedef struct {
    SnxThumbShape shape;
    unsigned op; /* operation within the shape */
    unsigned rd, rn, rm, ra;
    uint32_t imm;
    unsigned flags;
    unsigned len; /* TH_F16 or TH_F32 */
} SnxThumb;

/* Strictly decode one Thumb16 or Thumb32 instruction. hw2 is the second
   halfword (ignored for 16-bit forms). Returns the length in halfwords
   (1 or 2), or 0 for a reserved/unpredictable encoding. */
unsigned snx_thumb_decode(uint16_t hw1, uint16_t hw2, SnxThumb* out);
unsigned snx_thumb_decode_profile(SnxArmProfile profile, uint16_t hw1, uint16_t hw2, SnxThumb* out);

#endif
