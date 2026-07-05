/*
 * shinobi_dyntrans_amiga.c — ON-TARGET runtime of the Shinobi (Sega System 16B)
 * 68000 -> 68EC020 DYNAMIC BINARY TRANSLATOR.
 *
 * This is the Amiga counterpart of the proven host prototype tools/shinobi_dyntrans.c.
 * It shares the decode/emit core (tools/shinobi_xlate.c) with the host validator
 * tools/shinobi_xlate_test.c, which proves that core against Musashi byte-exact
 * over the runtime-reachable instruction set.  Here the SAME decoder is used to
 * EMIT real rebased 020 code into a code cache that the dispatch trampoline
 * (shinobi_dyntrans.s) executes natively.
 *
 * MODEL (see memory/shinobi-port-facts.md "AMIGA PORT DESIGN"):
 *   - guest address G lives at  base + G  in a flat 16MB fast-RAM block.  ROM is
 *     copied in at offset 0; tile/text/sprite/palette/work RAM and the I/O-shadow
 *     page are just regions of the same flat space.
 *   - a translated block copies each guest instruction VERBATIM, +base-patches
 *     its absolute EAs (widening abs.w->abs.l) and `move(a).l #imm,An` address
 *     immediates, and ends with a terminator stub that round-trips the dispatcher
 *     carrying the next GUEST pc (guest PCs/stack stay guest-consistent; only the
 *     live host PC differs).
 *   - I/O needs NO trapping: it is a shadow page the HAL fills (inputs/DSW/open
 *     bus) pre-frame and scans (sound latch / mapper cmd) post-frame.
 *   - IRQ4 (vblank, handler 0x2684) is synthesised each frame by pushing a 020
 *     format-0 exception frame onto the guest stack and dispatching 0x2684; the
 *     handler's `rte` terminator returns through the dispatcher.
 *
 * STUBBED for this first-light build (see report): the AGA renderer call
 * (shinobi_render, weak no-op here — a separate agent owns it) and the Z80 sound
 * core (only the latch byte is captured).  Real Amiga input mapping is a TODO
 * (inputs are held at "nothing pressed", matching the prototype's no-input run).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <stdint.h>
#include "shinobi_xlate.h"

/* ---- the program ROM, embedded by shinobi_romdata.s ---- */
extern const unsigned char shinobi_rom_main[];   /* 256KB flat big-endian image */
#define ROM_SIZE 0x40000u

/* ---- guest map constants ---- */
#define GUEST_SIZE     0x1000000u     /* full 24-bit guest space, flat           */
#define G_RESET_SSP    0xFFFFFF00u    /* longword @0 in ROM                       */
#define G_RESET_PC     0x000400u      /* longword @4 in ROM                       */
#define G_IRQ4_HANDLER 0x002684u      /* vblank handler entry                     */
#define FAULT_SENTINEL 0xDEADFA11u    /* g_pc value meaning "unsupported term"    */
#ifdef SHINOBI_DBGTRACE
/* The early alloc-status is written to fixed chip 0x1C0000 (readable before the
 * guest space exists).  The per-frame fingerprint, however, must live in memory
 * the live OS never reuses — chip 0x1C0000+64.. gets partially clobbered by OS
 * allocations.  So the per-frame fingerprint is published into our OWN AllocMem'd
 * z3 guest block at an UNUSED guest offset (0xE00000, between I/O and the mapper
 * window — the Sega game never touches it).  shinobi_dbgp points there. */
#define DBG_STATUS_ADDR 0x1C0000u
#define DBG_FP_OFFSET   0x00E00000u
volatile uint8_t *shinobi_dbgp = (volatile uint8_t*)0x1C0000u;  /* set in init */
#define DBG_FP_ADDR    ((uint32_t)(uintptr_t)shinobi_dbgp)
#endif

/* ====================================================================== */
/* GUEST REGISTER FILE (shared with shinobi_dyntrans.s).                    */
/* g_dregs = d0-d7,a0-a6 in movem order; g_sp = guest a7 (=base+SP).        */
/* ====================================================================== */
uint32_t g_dregs[15];
uint32_t g_sp;
uint32_t g_pc;
uint16_t g_sr;                        /* virtual guest SR system byte (S/IPL)     */
uint16_t g_ccr;                       /* CCR carried block-to-block (user-mode)   */
uint32_t g_fault_pc;
void    *g_host_sp;
void    *g_enter_target;

/* asm entry points (shinobi_dyntrans.s) */
extern void block_enter(void *host_block);
extern void exit_thunk(void);          /* address only — target of block stubs     */
extern void shinobi_super_reset(void); /* no-op now (user-mode translator)          */

/* USER-MODE i-cache flush: movec CACR is privileged, so flush via Exec CacheClearU()
 * (user-legal). Called after every block emit/patch for 020 i-cache coherency. */
void shinobi_clear_icache(void){ CacheClearU(); }

/* ====================================================================== */
/* FLAT GUEST SPACE + CODE CACHE (fast RAM).                                */
/* ====================================================================== */
static uint8_t *guest_base;           /* base of the 16MB flat guest space        */
#define CC_SIZE  (3u*1024u*1024u)     /* code cache size                          */
static uint8_t *cc_base;
static uint32_t cc_used;

#define CCH_BITS 15
#define CCH      (1u<<CCH_BITS)
struct cent { uint32_t gpc; uint8_t *host; };
static struct cent cctab[CCH];        /* open-addressing guest_pc -> host code     */

static xl_emit_env EENV;
long shinobi_blocks_translated;       /* diagnostics (read by HAL)                */
long shinobi_dispatches;
long shinobi_faults;
uint32_t shinobi_last_fault_pc;

static unsigned cc_hash(uint32_t pc){ return ((pc*2654435761u)>>11) & (CCH-1); }

static uint8_t *cc_lookup(uint32_t pc){
    unsigned h=cc_hash(pc);
    while (cctab[h].host){ if (cctab[h].gpc==pc) return cctab[h].host; h=(h+1)&(CCH-1); }
    return 0;
}
static void cc_insert(uint32_t pc, uint8_t *host){
    unsigned h=cc_hash(pc);
    while (cctab[h].host){ if (cctab[h].gpc==pc) return; h=(h+1)&(CCH-1); }
    cctab[h].gpc=pc; cctab[h].host=host;
}

/* ---- flat guest poke/peek helpers (big-endian, like the real bus) ---- */
static void poke16(uint32_t a, uint16_t v){ guest_base[a]=v>>8; guest_base[a+1]=v; }
static void poke32(uint32_t a, uint32_t v){
    guest_base[a]=v>>24; guest_base[a+1]=v>>16; guest_base[a+2]=v>>8; guest_base[a+3]=v;
}

/* ====================================================================== */
/* BLOCK TRANSLATOR — emit a rebased 020 block for guest pc, cache it.      */
/* ====================================================================== */
#define BLOCK_INSN_CAP 256

static uint8_t *translate(uint32_t pc){
    /* reset the cache if nearly full (rare in attract; full game would need a
     * smarter eviction — documented as a TODO).  Clearing the table + icache is
     * safe because no block references another block's host address directly
     * (all inter-block control flow goes through the dispatcher by guest pc). */
    if (cc_used + 4096 > CC_SIZE){
        cc_used = 0;
        for (unsigned i=0;i<CCH;i++){ cctab[i].host=0; cctab[i].gpc=0; }
    }

    uint8_t *host = cc_base + cc_used;
    uint8_t *o = host;
    int room = (int)(CC_SIZE - cc_used);
    int produced = 0;
    uint32_t a = pc;
    int insns = 0;

    for (;;){
        xdec d;
        xl_decode(guest_base, GUEST_SIZE, a, &d);
        if (d.term==XT_NONE){
            int e = xl_emit_instr(guest_base, GUEST_SIZE, a, &d, &EENV, o+produced, room-produced);
            if (e<=0){ /* overflow or undecodable body: fault out */
                int f = xl_emit_fault(a, &EENV, o+produced, room-produced);
                produced += (f>0)?f:0; break;
            }
            produced += e;
            a += d.len;
            if (++insns >= BLOCK_INSN_CAP){
                /* size cap: emit an unconditional dispatch to the next pc */
                xdec fd; fd.term=XT_BRA; fd.stgt=a;
                int e2 = xl_emit_term(a, &fd, a, &EENV, o+produced, room-produced);
                produced += (e2>0)?e2:0; break;
            }
            continue;
        }
        /* terminator */
        int e = xl_emit_term(a, &d, a + d.len, &EENV, o+produced, room-produced);
        if (e<=0){
            int f = xl_emit_fault(a, &EENV, o+produced, room-produced);
            produced += (f>0)?f:0;
        } else produced += e;
        break;
    }

    cc_used += produced;
    cc_insert(pc, host);
    shinobi_clear_icache();           /* 020 i-cache coherency after emit          */
    shinobi_blocks_translated++;
    return host;
}

static uint8_t *lookup_or_translate(uint32_t pc){
    uint8_t *h = cc_lookup(pc);
    return h ? h : translate(pc);
}

/* ====================================================================== */
/* I/O SHADOW — pre-frame fills / post-frame scans.                        */
/* ====================================================================== */
static uint8_t shinobi_in_p1=0xff, shinobi_in_p2=0xff, shinobi_in_svc=0xff;
static uint8_t shinobi_dsw1=0xff,  shinobi_dsw2=0xff;
uint8_t  shinobi_sound_latch;         /* last sound-latch byte (for the Z80 core)  */
uint8_t  shinobi_mapper_table[16];    /* captured 315-5195 region table            */

static void io_prefill(void){
    /* inputs (custom_io read region 0xC41001/03/07, 0xC42001/03) */
    guest_base[0xC41001] = shinobi_in_p1;
    guest_base[0xC41003] = shinobi_in_svc;
    guest_base[0xC41007] = shinobi_in_p2;
    guest_base[0xC42001] = shinobi_dsw1;
    guest_base[0xC42003] = shinobi_dsw2;
    /* open-bus regions the game samples as 0xFFFF */
    poke16(0xC60000, 0xFFFF);          /* watchdog                                  */
    poke16(0x3F0000, 0xFFFF);          /* mapper cmd window (game pulses it /frame) */
}

static void io_postscan(void){
    shinobi_sound_latch = guest_base[0xC43001];
    for (int i=0;i<16;i++) shinobi_mapper_table[i] = guest_base[0xFE0020 + i*2 + 1];
}

/* ====================================================================== */
/* IRQ4 SYNTHESIS — push a 020 format-0 exception frame, dispatch 0x2684.   */
/* Gated on the guest interrupt mask (IPL < 4), matching the prototype's    */
/* m68k_set_irq(4) which Musashi only honours when SR.IPL < 4.              */
/* ====================================================================== */
#ifdef SHINOBI_DBGTRACE
/* on-target fingerprint counters (see dbgtrace_dump) */
static uint32_t dbg_frame;
static uint32_t dbg_irq4_served;
static uint32_t dbg_irq4_attempts;
#endif

static void inject_irq4(void){
#ifdef SHINOBI_DBGTRACE
    dbg_irq4_attempts++;
#endif
    unsigned ipl = (g_sr >> 8) & 7;
    if (ipl >= 4) return;              /* masked: skip this frame (game still in init)*/
#ifdef SHINOBI_DBGTRACE
    dbg_irq4_served++;                 /* count IRQ4s actually injected (== handler entries) */
#endif
    /* g_sp is a HOST-ABSOLUTE pointer (= guest_base + guest_SP; it is what the
     * trampoline loads straight into A7).  Write the frame to that absolute
     * address — do NOT route through poke16/poke32, which re-add guest_base and
     * would doubly-base the write off the guest stack (the guest handler's rte
     * would then pop a garbage PC).  The 020 is big-endian, so native stores lay
     * the bytes out exactly as the guest expects. */
    uint32_t sp = g_sp;
    sp -= 6;                           /* 68000 exception frame = [SR.w][PC.l]      */
    *(volatile uint16_t*)(sp+0) = g_sr;     /* [SP+0] saved SR                      */
    *(volatile uint32_t*)(sp+2) = g_pc;     /* [SP+2] resume PC                     */
    g_sp = sp;
    g_sr = (uint16_t)(0x2400 | (g_sr & 0x00FF)); /* S=1, IPL=4, keep CCR            */
    g_pc = G_IRQ4_HANDLER;
}

/* ====================================================================== */
/* DISPATCH                                                                 */
/* ====================================================================== */
#ifdef SHINOBI_DBGTRACE
static uint32_t dbg_disp_iters;        /* monotonic dispatch-loop iteration count   */
static int      dbg_first_fault_set;
static uint32_t dbg_first_fault_pc;
static uint32_t dbg_first_fault_w0;
static uint32_t dbg_last_good_pc;      /* last in-range guest pc before a fault     */
static uint32_t dbg_prev_pc;           /* the one before that (who jumped to it)     */
static uint32_t dbg_fault_from_pc;     /* dbg_prev_pc captured at first fault        */
#endif
#ifdef SHINOBI_DBGTRACE
/* ---- dispatch PC ring (differential trace vs the host oracle) ----
 * Records every dispatched guest block PC into a ring at DBG_FP_ADDR+0x800
 * (longword[0] = write cursor, then 1023 PC slots).  Dump it off-target and read
 * the stuck attract sub-loop's block sequence to localise the base-sensitive
 * mis-branch.  Also a coverage bitmap (one bit per ROM byte) at +0x3000 so we can
 * diff WHICH guest PCs we reach vs the host. */
#define DBG_RING_OFF   0x800u
#define DBG_RING_N     1023u
#define DBG_COV_OFF    0x3000u
static uint32_t dbg_ring_w;
static uint32_t dbg_ring_last = 0xFFFFFFFFu;
static void dbg_ring_push(uint32_t pc){
    if (pc < 0x40000u)
        ((volatile uint8_t*)(DBG_FP_ADDR + DBG_COV_OFF))[pc>>3] |= (uint8_t)(1u<<(pc&7));
    /* collapse consecutive-duplicate PCs (the vblank wait-spin) so the ring shows
     * the actual BLOCK FLOW, not 20000 copies of the spin block.  FREEZE once full
     * so it captures the FIRST 1023 blocks = the BOOT SEQUENCE (for a differential
     * first-divergence diff vs the host oracle's ordered boot trace). */
    if (pc == dbg_ring_last) return;
    dbg_ring_last = pc;
    if (dbg_ring_w >= DBG_RING_N) return;
    volatile uint32_t *ring = (volatile uint32_t*)(DBG_FP_ADDR + DBG_RING_OFF);
    ring[1 + dbg_ring_w] = pc;
    dbg_ring_w++;
    ring[0] = dbg_ring_w;
}
#endif
static void dispatch(long budget){
    while (budget-- > 0){
#ifdef SHINOBI_DBGTRACE
        /* live heartbeat: total loop iterations + the guest pc we are ABOUT to
         * enter.  If iters stops advancing across runs, block_enter(g_pc) never
         * returned (a hang); fp[22] then names the offending guest block. */
        *((volatile uint32_t*)(DBG_FP_ADDR + 84)) = ++dbg_disp_iters;
        *((volatile uint32_t*)(DBG_FP_ADDR + 88)) = g_pc;
#endif
        if (g_pc == FAULT_SENTINEL){
            shinobi_faults++;
            shinobi_last_fault_pc = g_fault_pc;
#ifdef SHINOBI_DBGTRACE
            if (!dbg_first_fault_set){
                dbg_first_fault_set = 1;
                dbg_first_fault_pc  = g_fault_pc;
                dbg_first_fault_w0  = ((uint32_t)guest_base[g_fault_pc & 0xFFFFFF]<<8)
                                    |  (uint32_t)guest_base[(g_fault_pc & 0xFFFFFF)+1];
                dbg_fault_from_pc   = dbg_prev_pc;
            }
#endif
            /* recover: skip the faulting instruction and continue (first-light
             * policy — STOP/illegal/unsupported computed jump).  A real build
             * would translate the computed-jump target instead. */
            g_pc = g_fault_pc + 2;
            continue;
        }
        /* Normalize a possibly-REBASED control-transfer target back to guest
         * space.  jmp/jsr(An) (and rts) can carry a base-relative pointer: a CODE
         * address computed via a rebased lea / movea.l #imm and stashed in a RAM
         * vtable or on the stack, then dispatched.  The terminator sets g_pc = An,
         * so g_pc = base+guest.  True guest PCs are < GUEST_SIZE < base, so only
         * genuine rebased values fall in [base, base+GUEST_SIZE). */
        if (g_pc >= (uint32_t)(uintptr_t)guest_base &&
            (uint32_t)(g_pc - (uint32_t)(uintptr_t)guest_base) < GUEST_SIZE)
            g_pc -= (uint32_t)(uintptr_t)guest_base;
#ifdef SHINOBI_DBGTRACE
        if (g_pc < GUEST_SIZE){ dbg_prev_pc = dbg_last_good_pc; dbg_last_good_pc = g_pc; }
#endif
#ifdef SHINOBI_DBGTRACE
        dbg_ring_push(g_pc);
#endif
        uint8_t *host = lookup_or_translate(g_pc);
        block_enter(host);
        shinobi_dispatches++;
    }
}

#ifdef SHINOBI_DBGTRACE
/* ====================================================================== */
/* ON-TARGET DEBUG FINGERPRINT (SHINOBI_DBGTRACE).                          */
/* Once per emulated frame, after the IRQ4 handler has run, scan the GUEST  */
/* VRAM (tile/text/sprite/palette) and write a compact fingerprint to a     */
/* FIXED CHIP-RAM address so copperline's COPPERLINE_DBG_RAMDUMP can read it */
/* off-target headlessly and compare to tools/shinobi_host.c's golden stats.*/
/* All fields are native big-endian (020) longwords; decode big-endian.     */
/*                                                                          */
/*   [0]  0x53484650 'SHFP'  magic                                          */
/*   [1]  frame counter (HAL frames run)                                    */
/*   [2]  IRQ4 served  (== host "handler 0x2684 entries")                   */
/*   [3]  blocks translated                                                 */
/*   [4]  dispatches                                                        */
/*   [5]  faults                                                            */
/*   [6]  last fault pc                                                     */
/*   [7]  tilemap  non-zero words  (guest 0x400000..0x40FFFF)               */
/*   [8]  textram  non-zero words  (guest 0x410000..0x410FFF)               */
/*   [9]  sprite   non-zero words  (guest 0x440000..0x4407FF)               */
/*   [10] palette  non-zero words  (guest 0x840000..0x840FFF)               */
/*   [11] current guest pc                                                  */
/*   [12..15] (bytes @ +48) captured 16-byte mapper table                   */
/* ====================================================================== */
/* early-boot diagnostics live at fp[16..19] (offset +64), written before the
 * VRAM fingerprint exists, so a still-zero magic can be told apart from a
 * failed AllocMem / a dispatch that never returns. */
static uint32_t dbg_raw_frames;
static void dbgtrace_init_status(uint32_t status){
    volatile uint32_t *fp = (volatile uint32_t*)DBG_STATUS_ADDR;
    fp[16] = 0xB007C0DEu;              /* "boot code" — init() definitely ran      */
    fp[17] = status;                  /* 2=guest+cc ok, 1=guest only, 0=alloc fail */
    fp[18] = (uint32_t)guest_base;
    fp[19] = (uint32_t)cc_base;
}

static void dbgtrace_dump(void){
    uint8_t *g = guest_base;
    uint32_t tile_nz=0, text_nz=0, spr_nz=0, pal_nz=0;
    uint32_t a;
    for (a=0x400000u; a<0x410000u; a+=2) if (g[a]||g[a+1]) tile_nz++;
    for (a=0x410000u; a<0x411000u; a+=2) if (g[a]||g[a+1]) text_nz++;
    for (a=0x440000u; a<0x440800u; a+=2) if (g[a]||g[a+1]) spr_nz++;
    for (a=0x840000u; a<0x841000u; a+=2) if (g[a]||g[a+1]) pal_nz++;

    volatile uint32_t *fp = (volatile uint32_t*)DBG_FP_ADDR;
    fp[0]  = 0x53484650u;
    fp[1]  = dbg_frame;
    fp[2]  = dbg_irq4_served;
    fp[3]  = (uint32_t)shinobi_blocks_translated;
    fp[4]  = (uint32_t)shinobi_dispatches;
    fp[5]  = (uint32_t)shinobi_faults;
    fp[6]  = shinobi_last_fault_pc;
    fp[7]  = tile_nz;
    fp[8]  = text_nz;
    fp[9]  = spr_nz;
    fp[10] = pal_nz;
    fp[11] = g_pc;
    volatile uint8_t *mp = (volatile uint8_t*)(DBG_FP_ADDR + 48);
    for (int i=0;i<16;i++) mp[i] = shinobi_mapper_table[i];
    fp[26] = dbg_first_fault_pc;        /* +104 */
    fp[27] = dbg_first_fault_w0;        /* +108 */
    fp[28] = dbg_fault_from_pc;         /* +112: guest block whose terminator derailed */
    fp[29] = g_sr;                      /* +116: current guest SR (IPL gate insight)  */
    fp[30] = dbg_irq4_attempts;         /* +120: inject_irq4 calls (== frames)        */
}
#endif /* SHINOBI_DBGTRACE */

/* renderer hook — owned by the AGA renderer agent.  Weak no-op default so this
 * runtime links + drives the guest to produce VRAM even before the renderer
 * exists.  guest_base + 0x400000/0x410000/0x440000/0x840000 hold the live
 * tile/text/sprite/palette RAM to scan. */
void shinobi_render(uint8_t *gbase) __attribute__((weak));
void shinobi_render(uint8_t *gbase){ (void)gbase; }

/* ====================================================================== */
/* PUBLIC HAL ENTRY POINTS                                                  */
/* ====================================================================== */
/* Block dispatches per HAL frame.  Each HAL frame == one guest vblank: we inject
 * one IRQ4 at the top, then dispatch.  The guest's per-vblank work (the 0x2684
 * handler + a slice of the main loop) is a few thousand blocks; once it reaches a
 * vblank wait-spin (clr/tst $f01c.w) it can only be released by the NEXT frame's
 * IRQ4, so any budget spent spinning past that point is pure waste (and very slow
 * in emulated time).  Keep the budget just large enough to cover the handler plus
 * the inter-wait work, so the spin burns only a small tail.  (Tuning knob — see
 * shinobi-port-facts risk list.) */
#define FRAME_BUDGET 24000L

int shinobi_dyntrans_init(void){
    guest_base = (uint8_t*)AllocMem(GUEST_SIZE, MEMF_FAST|MEMF_CLEAR);
    if (!guest_base) guest_base = (uint8_t*)AllocMem(GUEST_SIZE, MEMF_ANY|MEMF_CLEAR);
    if (!guest_base){
#ifdef SHINOBI_DBGTRACE
        dbgtrace_init_status(0);        /* guest alloc failed                       */
#endif
        return 0;                       /* needs ~16MB fast RAM (accelerated A1200) */
    }
    cc_base = (uint8_t*)AllocMem(CC_SIZE, MEMF_FAST|MEMF_CLEAR);
    if (!cc_base) cc_base = (uint8_t*)AllocMem(CC_SIZE, MEMF_ANY|MEMF_CLEAR);
    if (!cc_base){
#ifdef SHINOBI_DBGTRACE
        dbgtrace_init_status(1);        /* guest ok, code-cache alloc failed        */
#endif
        FreeMem(guest_base, GUEST_SIZE); return 0;
    }
#ifdef SHINOBI_DBGTRACE
    shinobi_dbgp = guest_base + DBG_FP_OFFSET;  /* publish fingerprint in our z3 RAM */
    dbgtrace_init_status(2);            /* both allocations OK (chip 0x1C0000)      */
#endif

    /* copy the program ROM into the flat guest space at offset 0 */
    for (uint32_t i=0;i<ROM_SIZE;i++) guest_base[i] = shinobi_rom_main[i];

    EENV.base           = (uint32_t)guest_base;
    EENV.gregs_pc       = (uint32_t)&g_pc;
    EENV.gregs_sr       = (uint32_t)&g_sr;
    EENV.gregs_ccr      = (uint32_t)&g_ccr;
    EENV.exit_thunk     = (uint32_t)&exit_thunk;
    EENV.fault_pc       = (uint32_t)&g_fault_pc;
    EENV.fault_sentinel = FAULT_SENTINEL;

    /* seed reset state from the ROM vectors, rebased into the flat space */
    g_sp = (G_RESET_SSP & 0xFFFFFF) + (uint32_t)guest_base;
    g_pc = G_RESET_PC;
    g_sr = 0x2700;                      /* supervisor, IPL=7 (matches boot)         */
    for (int i=0;i<15;i++) g_dregs[i]=0;

    cc_used = 0;
    return 1;
}

void shinobi_dyntrans_set_inputs(uint8_t p1, uint8_t p2, uint8_t svc,
                                 uint8_t dsw1, uint8_t dsw2){
    shinobi_in_p1=p1; shinobi_in_p2=p2; shinobi_in_svc=svc;
    shinobi_dsw1=dsw1; shinobi_dsw2=dsw2;
}

void shinobi_dyntrans_frame(void){
#ifdef SHINOBI_DBGTRACE
    /* raw frame-entry beat, written BEFORE dispatch — if dbg_frame (fp[1]) lags
     * this, the translator is stuck inside dispatch() of that frame. */
    *((volatile uint32_t*)(DBG_FP_ADDR + 80)) = ++dbg_raw_frames;
#endif
    /* CRITICAL: the guest runs natively with its OWN SR live on the 020, and it
     * lowers its interrupt mask to IPL3 (its Sega vblank level) during the frame.
     * On the Amiga that UNMASKS real chip interrupts (Paula audio = level 4, CIA =
     * level 6, ...), which would vector through the Amiga VBR into OS handlers
     * running on the GUEST stack mid-block — corrupting host state and wedging the
     * per-frame Supervisor() call.  The guest's vblank is delivered purely in
     * software (inject_irq4), so disable ALL Amiga interrupts for the duration of
     * the guest frame and restore the previous mask afterwards. */
    volatile uint16_t *INTENA  = (volatile uint16_t*)0xDFF09Au;
    volatile uint16_t *INTENAR = (volatile uint16_t*)0xDFF01Cu;
    uint16_t saved_intena = *INTENAR;
    *INTENA = 0x7FFF;                   /* clear master + all sources             */

    io_prefill();
    inject_irq4();
    dispatch(FRAME_BUDGET);
    io_postscan();

    *INTENA = 0x8000 | (saved_intena & 0x7FFF);  /* restore the OS interrupt mask */
#ifdef SHINOBI_DBGTRACE
    dbg_frame++;
    dbgtrace_dump();                   /* publish the on-target VRAM fingerprint */
#endif
    shinobi_render(guest_base);
#ifdef SHINOBI_DBGTRACE
    /* end-of-frame beat: written AFTER everything in the frame returns.  If this
     * tracks dbg_frame, the frame fully returned to the HAL loop (no tail hang). */
    *((volatile uint32_t*)(DBG_FP_ADDR + 92)) = dbg_frame;
#endif
    shinobi_super_reset();             /* clean supervisor SR so Supervisor() returns */
}

uint8_t *shinobi_dyntrans_guest_base(void){ return guest_base; }
