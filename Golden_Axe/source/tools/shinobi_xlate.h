/*
 * shinobi_xlate.h — SHARED translation core for the Shinobi (Sega System 16B)
 * 68000 -> 68EC020 dynamic binary translator.
 *
 * This header + shinobi_xlate.c are the ONE piece of logic shared between:
 *   - the HOST validator  tools/shinobi_xlate_test.c  (proves the decoder against
 *     Musashi's m68kdasm over the runtime-reachable block set), and
 *   - the AMIGA runtime    src/hal/shinobi_dyntrans_amiga.c (emits real rebased
 *     020 code into the on-target code cache).
 *
 * It contains NO Musashi and NO Amiga dependencies — pure C99, libc-free except
 * for the caller-supplied flat guest memory pointer.  The decoder is a minimal
 * 68000 instruction LENGTH + EA classifier (enough to copy instructions, locate
 * the absolute EAs that must be rebased, and detect the block terminator); the
 * emitter copies each instruction verbatim, +base-patches its absolute EAs
 * (widening abs.w -> abs.l), and appends a terminator stub that round-trips the
 * dispatcher with the next GUEST pc.
 */
#ifndef SHINOBI_XLATE_H
#define SHINOBI_XLATE_H

#include <stdint.h>

/* ---- terminator classes (mirror tools/shinobi_dyntrans.c Term order) ---- */
enum {
    XT_NONE = 0,
    XT_BRA, XT_BCC, XT_BSR, XT_DBCC,     /* static relative targets         */
    XT_JMP_ABS, XT_JSR_ABS,              /* static absolute targets         */
    XT_JMP_PCREL, XT_JSR_PCREL,          /* PC-relative computed jump table  */
    XT_JMP_IND, XT_JSR_IND,              /* DYNAMIC vtable jmp/jsr (An)       */
    XT_RTS, XT_RTR, XT_RTE,              /* DYNAMIC return (target on stack) */
    XT_STOP,                             /* stop/reset/trap/illegal          */
    XT_FALL,                             /* hit block size cap, falls through*/
    XT_UNCL,                             /* could not decode                 */
    XT_N
};
extern const char *const xt_name[XT_N];
int xt_is_dynamic(int t);
int xt_is_vtable(int t);

/* ---- per-instruction decode result ---- */
#define XL_MAXABS 4
enum { XF_ABSW = 1, XF_ABSL, XF_IMM_AN, XF_PCREL16, XF_IMM_AN_W }; /* fixup kinds */
/* XF_IMM_AN_W: `movea.w #imm16,An` — the word immediate sign-extends to a 24-bit
 * guest ADDRESS (e.g. #$c000 -> $ffc000 work RAM) loaded into an address reg, so
 * it must be rebased AND the instruction widened to `movea.l #imm32,An`. */
/* XF_PCREL16: a (d16,PC) DATA effective address.  When the instruction is
 * copied to a different host address the PC base changes, so we resolve it
 * statically to base + ((pc + woff) + sign16(disp)) and widen it to abs.l,
 * exactly like XF_ABSW (the source EA field 7/2 -> 7/1, +2 bytes).          */
enum { XFLD_SRC = 0, XFLD_DST };            /* which EA field (for widening)*/

typedef struct {
    uint32_t len;            /* total instruction byte length (original)     */
    uint16_t w0;             /* the opcode word (for cc / reg reconstruction)*/
    uint16_t w1;             /* first extension word (disp etc.)             */
    int      term;           /* XT_* terminator class                        */
    uint32_t stgt;           /* static target (relative/abs) or 0xFFFFFFFF   */
    int      nabs;           /* number of absolute EAs to rebase             */
    struct {
        uint8_t woff;        /* byte offset within instr to this operand's
                                first extension word                          */
        uint8_t kind;        /* XF_ABSW / XF_ABSL / XF_IMM_AN                 */
        uint8_t field;       /* XFLD_SRC / XFLD_DST (abs.w widening)          */
    } abs[XL_MAXABS];
    uint8_t  tmode, treg;    /* terminator EA mode/reg (jmp/jsr indirect)    */
} xdec;

/* Decode the single instruction at guest address pc out of the flat guest
 * memory image `g` (big-endian, as stored in ROM). Fills *d. Returns d->len.
 * If undecodable, term=XT_UNCL and len is a safe 2. */
uint32_t xl_decode(const uint8_t *g, uint32_t gsize, uint32_t pc, xdec *d);

/* ---- emitter environment: runtime addresses the terminator stubs need ---- */
typedef struct {
    uint32_t base;        /* flat guest-space base added to absolute EAs      */
    uint32_t gregs_pc;    /* &g_pc   (32-bit cell: next guest pc)             */
    uint32_t exit_thunk;  /* address of the asm exit thunk (saves regs, rts)  */
    uint32_t gregs_sr;    /* &g_sr   (16-bit cell: virtual guest SR system byte) */
    uint32_t gregs_ccr;   /* &g_ccr  (16-bit cell: CCR carried block-to-block)  */
    uint32_t fault_pc;    /* &g_fault_pc (guest pc of an unsupported term)    */
    uint32_t fault_sentinel; /* value stored in g_pc to signal a fault        */
} xl_emit_env;

/* Emit ONE translated instruction (body, i.e. non-terminator) into out (cap
 * bytes). Returns emitted byte count, or 0 on overflow / undecodable. */
int xl_emit_instr(const uint8_t *g, uint32_t gsize, uint32_t pc,
                  const xdec *d, const xl_emit_env *env,
                  uint8_t *out, int cap);

/* Emit the terminator stub for a block whose terminator instruction is at pc
 * (decoded into *d). `fall` is the guest fall-through pc (pc+len). Returns
 * emitted byte count, or 0 if this terminator is not emittable here (STOP /
 * PC-relative computed jump / complex (d8,An,Xn) indirect) — the runtime then
 * emits a fault stub via xl_emit_fault(). */
int xl_emit_term(uint32_t pc, const xdec *d, uint32_t fall,
                 const xl_emit_env *env, uint8_t *out, int cap);

/* Emit a fault stub: records `pc` in g_fault_pc, sets g_pc to the fault
 * sentinel, and exits to the dispatcher (which logs/halts). Used for
 * unsupported terminators. Returns byte count. */
int xl_emit_fault(uint32_t pc, const xl_emit_env *env, uint8_t *out, int cap);

#endif /* SHINOBI_XLATE_H */
