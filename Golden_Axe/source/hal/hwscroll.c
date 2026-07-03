/* src/hal/hwscroll.c -- generic Amiga AGA hardware-scroll engine (1 or 2 layers).
 * See hwscroll.h. Folds the proven single-playfield smooth scroll + dual-playfield
 * parallax (DBLPF + BPLCON3 PF2 colour offset + interleaved planes + per-layer
 * fine/coarse scroll with the extra-fetch-word setup). */
#include <exec/exec.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <graphics/gfxbase.h>
#include "hwscroll.h"

struct GfxBase *GfxBase = 0;   /* graphics.library protos (LoadView/WaitTOF) use this */

/* Extra INTENA bits to re-assert AFTER each INTENA=0x7FFF mask. The takeover masks
 * ALL interrupts (so the OS keyboard handler can't eat coin/start keys), but an
 * interrupt-driven audio replayer (ptplayer, CIA-B/EXTER) needs its interrupt kept
 * alive. A consumer sets this to 0xE000 (SET|INTEN|EXTER) to keep master+EXTER on
 * without re-enabling the OS keyboard/vblank interrupts. Default 0 = old behaviour
 * (all interrupts stay masked), so other games are unaffected. */
volatile uint16_t hwscroll_intena_keep = 0;

/* Opt-in: drop the horizontal fine-scroll PRE-ROLL fetch word. The default setup
 * fetches ONE extra bitplane word (DDFSTRT-8, modulo-2) so BPLCON1 horizontal fine
 * scroll has slack. That pre-roll word is fetched one colour-clock EARLY and is
 * blanked by the display window -- but ONLY on cycle-exact / real DDF-DIW timing
 * (the actual device). On lenient emulation (Amiberry, cpu_cycle_exact=false) it is
 * harmless, which is why it looked fine in-emulator yet showed an off-by-one-column
 * LEFT margin on the device. For games that NEVER horizontally fine-scroll (portrait
 * vertical scrollers: Terra Cresta, 1943, Commando -- they always call hwscroll_set
 * with x=0, so BPLCON1 horiz nibble is always 0), the pre-roll word is pure dead
 * weight. Set this to 1 BEFORE hwscroll_open to use the canonical aligned setup
 * (DDFSTRT = (DIWSTRT-0x11)/2, exact column-0-at-left-edge), which is identical on
 * lenient emulation and CORRECT on cycle-exact hardware. Default 0 = unchanged. */
volatile int hwscroll_no_preroll = 0;

/* Opt-in: SYMMETRIC vertical crop of the displayed window (long/portrait axis).
 * The vertical display window is otherwise fixed at the canonical 256 lines
 * (DIWSTRT V=0x2C .. DIWSTOP V=0x12C). A portrait arcade port whose true visible
 * frame is exactly 256 lines (Terra Cresta, 1943, Commando) shows the full window;
 * but on some real devices the very top and very bottom display row fall in the
 * monitor/scaler's overscan and show as extra/garbage rows beyond the arcade frame.
 * Set these (lines to drop off the top / bottom) BEFORE hwscroll_open to tighten
 * the vertical window by that many lines at each end. The bitplane start row is
 * advanced by hwscroll_vcrop_top so content is CROPPED (not squashed) and the frame
 * stays vertically CENTRED (a symmetric top==bot crop preserves the original centre).
 * Default 0/0 = the full 256-line window, so games that don't set them are unchanged. */
volatile int hwscroll_vcrop_top = 0;
volatile int hwscroll_vcrop_bot = 0;

/* Opt-in: HORIZONTAL window nudge (lores px) added to the centred DIWSTRT hstart.
 * The display window is otherwise centred on the standard Amiga 320-px display
 * (hstart = 0x81 + (320-disp_w)/2, so its centre == the Amiga display centre).
 * That IS mathematically centred -- but a host/emulator whose visible capture
 * region is itself off-centre (Amiberry with gfx_center_horizontal=none captures
 * a region centred a few colour-clocks LEFT of the Amiga display centre) makes the
 * perfectly-centred window look shifted RIGHT (bigger left margin). Set this (signed
 * lores px, negative = nudge the window/playfield LEFT) BEFORE hwscroll_open to bias
 * the window so it reads visually centred in that capture. DDFSTRT follows hstart, so
 * keep the value EVEN (hstart stays odd => ddf=(hstart-0x11)/2 is exact and the
 * playfield content stays flush at the window's left edge). The sprite HSTART base is
 * shifted with it so hardware sprites still align. Default 0 = unchanged (every other
 * game keeps the canonical centred window). */
volatile int hwscroll_hwin_adj = 0;

#define CUSTOM   ((volatile uint16_t *)0xdff000)
#define R_DMACON  (0x096/2)
#define R_DIWSTRT (0x08E/2)
#define R_DIWSTOP (0x090/2)
#define R_DDFSTRT (0x092/2)
#define R_DDFSTOP (0x094/2)
#define R_BPLCON0 (0x100/2)
#define R_BPLCON1 (0x102/2)
#define R_BPLCON2 (0x104/2)
#define R_BPLCON3 (0x106/2)
#define R_BPLCON4 (0x10C/2)
#define R_BPL1MOD (0x108/2)
#define R_BPL2MOD (0x10A/2)
#define R_COLOR00 (0x180/2)
#define R_COP1LCH (0x080/2)
#define R_VPOSR   (0x004/2)
#define R_INTENA  (0x09A/2)
#define R_INTREQ  (0x09C/2)

static int s_scrollx[HWS_MAXLAYERS], s_scrolly[HWS_MAXLAYERS], s_fine[HWS_MAXLAYERS];

/* Copper-list word index of the BPLCON1 (horizontal fine-scroll) MOVE. The fine
 * scroll is driven from the COPPER -- NOT a direct custom-register poke -- so it
 * is applied at the top of the frame, ATOMICALLY with the coarse bitplane-pointer
 * MOVEs that also live in the copper list. The old code poked BPLCON1 directly in
 * hwscroll_set (applied immediately, mid-frame) while the coarse pointers only
 * took effect at the next copper restart: at every 16px boundary the fine nibble
 * (15->0) and the coarse +1-word handoff landed on DIFFERENT frames, so the image
 * jumped 16px for one frame -> the periodic scroll jitter. Co-locating both in the
 * copper makes the whole scroll value apply on one frame, tear-free. */
static int s_bplcon1_slot = -1;

static int s_sprptr_slot = -1;     /* copper word index of the 8 SPRxPT MOVEs */
static int s_fg_split = 0, s_fg_split_layer = 0, s_fg_split_scroll = 0;
static int s_fg_split_ytop = 0, s_fg_split_ybot = 0;   /* DISPLAY lines */

/* ---- configurable hardware-sprite pixel WIDTH (AGA FMODE) -------------------
 * Default 16px (classic OCS/ECS fetch -- byte-identical to the original engine, so
 * games that don't opt in are unchanged). Build a game with -DHWS_SPR_W=32 or 64 to
 * use AGA WIDE sprites: a 64px attached-pair sprite covers FOUR adjacent 16px tiles in
 * one channel, so wide objects (bosses) stop starving the 8-sprite / 4-pair budget.
 * Verified AGA wide-sprite memory layout (HRM + practice):
 *   - each control word sits in a fetch-wide slot: POS at +0, CTL at +HWS_SPR_WORDS
 *     (the 6 pad bytes after each control word in 64px mode stay 0 from MEMF_CLEAR)
 *   - each scanline = SPRxDATA (W bits) then SPRxDATB (W bits)
 *   - whole structure 8-byte aligned (holds here: plane_sz, SPR1, SPRBLK are mult of 4w)
 * FMODE sprite-fetch bits (3:2): 16px=00, 32px=01(0x4), 64px=11(0xC). */
#ifndef HWS_SPR_W
#define HWS_SPR_W 16
#endif
#define HWS_SPR_WORDS (HWS_SPR_W/16)            /* fetch words per plane per line (1/2/4)  */
#define HWS_SPR_CTLW  (2*HWS_SPR_WORDS)         /* control slot: POS(+pad) then CTL(+pad)  */
#define HWS_SPR_LINEW (2*HWS_SPR_WORDS)         /* data words per line: DATA then DATB      */
#define HWS_FMODE_SPR ((HWS_SPR_WORDS==1)?0x0000 : (HWS_SPR_WORDS==2)?0x0004 : 0x000C)
#define R_FMODE   (0x1FC/2)

#define SPR1      (HWS_SPR_CTLW + HWS_SPR_H*HWS_SPR_LINEW)  /* words for one sprite in a chain */
#define SPRBLK    (HWS_SPR_SLOTS*SPR1 + HWS_SPR_CTLW)       /* per-channel block: SLOTS + end  */
#define SPR_BASE_H 0x80                 /* sprite HSTART for screen x=0 (tuned)       */
#define SPR_BASE_V 0x2C                 /* sprite VSTART for screen y=0 (= DIWSTRT V)  */

/* multiplex state: per-channel write cursor (words) + next-free screen-y */
static int s_chan_cur[HWS_NSPR], s_chan_lasty[HWS_NSPR];

/* ---- attached-pair (15-colour) hw sprite + copper-colour-reload state ---- */
#define HWS_MAXREL   24          /* max per-frame copper palette reloads (distinct bands)*/
#define ASPR_RELWORDS 32         /* copper words one reload costs: WAIT(2) + 15 MOVEs(30)*/
#define HWS_FGSPLIT_WORDS 64       /* copper reserve for the 2 FG-split repositions */
/* Optional per-frame "top reposition" of ONE layer (e.g. the dual-pf back playfield):
 * re-latch its bitplane pointers + fine scroll at a copper WAIT just before the first
 * display line, instead of the frame-top (no-WAIT) BPLCON1 slot. The frame-top fine
 * nibble for a hardware-scrolled PF2 is applied during vblank with no raster gate; on
 * lenient emulation that can read back a 1px-unstable scroll every frame (a continuous
 * whole-layer shimmer) even though the value is correct -- the FG play band avoids it
 * precisely because emit_fg_repos() re-applies it at a WAIT. This region gives the same
 * WAIT-gated latch to any layer. Reserve = WAIT(2)+8 ptr MOVEs(32 max)+BPLCON1(2), say 40.
 * NOP-initialised in open(): a game that never calls hwscroll_layer_repos() pays nothing. */
#define HWS_LREPOS_WORDS 40
/* Single-playfield HUD band-split copper reserve: TWO mid-screen reloads, each a
 * WAIT (up to 4 words for the >255-line two-step) + up to 8 plane-ptr MOVEs (32) +
 * a BPLCON1 MOVE (2) = 38 words; 80 covers both with NOP slack. NOP-initialised in
 * open() so an engine that never calls hwscroll_hud_bands() pays nothing. */
#define HWS_HUDSPLIT_WORDS 80
static int s_hudsplit = -1;      /* copper word index of the HUD band-split region        */
static int s_lrepos = -1;        /* copper word index of the layer-repos region          */
static int s_relbase;            /* copper word index where the dynamic reload list starts */
static int s_spr_base_h;         /* sprite HSTART for screen x=0 (aligned to the DIW)   */
static int      s_hud_pending;
static uint8_t *s_hud_bg_bpl, *s_hud_hud_bpl;
static long     s_hud_plane_sz;
static int      s_hud_bg_v, s_hud_play_top, s_hud_play_bot;
static int s_apair_cur[HWS_ASPR_PAIRS];    /* per-pair chain write cursor (words)       */
static int s_apair_lasty[HWS_ASPR_PAIRS];  /* per-pair next-free screen y               */
static int s_aspr_n;                       /* sprites placed in hardware this frame     */
/* placed-sprite log (for vertical-overlap palette-conflict test + copper reloads),
 * fed in ascending-y order. */
static int      s_aspr_py[HWS_ASPR_PAIRS*HWS_ASPR_SLOTS];
static int      s_aspr_pid[HWS_ASPR_PAIRS*HWS_ASPR_SLOTS];
static uint16_t s_aspr_pal[HWS_ASPR_PAIRS*HWS_ASPR_SLOTS][15];

/* total hardware bitplanes = nlayers * pfplanes. */
static uint16_t bplcon0_for(hwscroll_t *s){
    int planes = s->nlayers * s->pfplanes;
    uint16_t v = (planes>=8) ? 0x0211 : (uint16_t)(0x0201 | (planes<<12));
    if (s->nlayers==2) v |= 0x0400;     /* DBLPF */
    return v;
}

/* compute a layer's pointer byte-offset from its scroll (ceil coarse + vertical). */
static long layer_byteoff(hwscroll_t *s, int layer){
    int word=(s_scrollx[layer]+15)>>4;
    /* +hwscroll_vcrop_top: advance the start row so a top vertical crop removes the
     * top N content rows (rather than just shifting the image up). Default 0. */
    return (long)(s_scrolly[layer]+hwscroll_vcrop_top)*s->stride + (long)word*2;
}

/* rebuild all hardware bitplane pointers in the copper from current scroll. */
static void set_ptrs(hwscroll_t *s){
    int total = s->nlayers * s->pfplanes;
    for (int hp=0; hp<total; hp++){
        int layer = (s->nlayers==2) ? (hp&1) : 0;
        int plane = (s->nlayers==2) ? (hp>>1) : hp;
        uint32_t a=(uint32_t)(s->bpl[layer] + plane*s->plane_sz + layer_byteoff(s,layer));
        s->copper[hp*4+0]=(uint16_t)(0x00E0+hp*4); s->copper[hp*4+1]=(uint16_t)(a>>16);
        s->copper[hp*4+2]=(uint16_t)(0x00E2+hp*4); s->copper[hp*4+3]=(uint16_t)(a&0xFFFF);
    }
    /* end marker is written once in open(), after the sprite pointers. */
}

int hwscroll_open(hwscroll_t *s, int nlayers, int pfplanes,
                  int disp_w, int disp_h, int buf_w, int buf_h){
    volatile uint16_t *c=CUSTOM;
    s->ok=0; s->nlayers=nlayers; s->pfplanes=pfplanes;
    s->disp_w=disp_w; s->disp_h=disp_h; s->buf_w=buf_w; s->buf_h=buf_h;
    s->stride=buf_w/8; s->plane_sz=(long)s->stride*buf_h;
    int total=nlayers*pfplanes;
    for (int i=0;i<HWS_MAXLAYERS;i++){ s_scrollx[i]=s_scrolly[i]=s_fine[i]=0; }
    GfxBase=(struct GfxBase*)OpenLibrary((CONST_STRPTR)"graphics.library",0);
    if (GfxBase){ LoadView(0); WaitTOF(); WaitTOF(); }
    /* copper: bpl ptrs + spr ptrs + ATTACHED-PAIR per-band colour reload region + end.
     * The reload region (s_relbase..) is rewritten each frame by hwscroll_aspr_finish;
     * open() seeds it with just an end marker so an unused engine costs nothing. */
    long copsz=(total*4 + 2 + HWS_NSPR*4 + HWS_LREPOS_WORDS + HWS_HUDSPLIT_WORDS + HWS_MAXREL*ASPR_RELWORDS + HWS_FGSPLIT_WORDS + 2)*2;
    long sprsz=(long)HWS_NSPR*SPRBLK*2;
    void *chunk=AllocMem((unsigned long)((long)total*s->plane_sz)+2*sprsz+copsz, MEMF_CHIP|MEMF_CLEAR);
    if(!chunk) return 0;
    uint8_t *p=(uint8_t*)chunk;
    for (int L=0;L<nlayers;L++){ s->bpl[L]=p; p += (long)pfplanes*s->plane_sz; }
    s->spr_buf[0]=(uint16_t*)p; p += sprsz;
    s->spr_buf[1]=(uint16_t*)p; p += sprsz;
    s->spr_back=0; s->spr=s->spr_buf[0];
    s->copper=(uint16_t*)p;
    s_fg_split=0;
    set_ptrs(s);
    /* BPLCON1 fine-scroll MOVE, right after the bitplane pointers (both execute at
     * frame top before the first display line). hwscroll_set updates word+1 only. */
    s_bplcon1_slot = total*4;
    s->copper[s_bplcon1_slot+0]=0x0102; s->copper[s_bplcon1_slot+1]=0;
    /* sprite pointers (point at each 36-word block) + copper end. Blocks start
     * zeroed = sprites inactive until hwscroll_sprite() sets them. */
    s_sprptr_slot = total*4 + 2;
    for (int i=0;i<HWS_NSPR;i++){
        uint32_t a=(uint32_t)(s->spr + i*SPRBLK); int b=total*4 + 2 + i*4;
        s->copper[b+0]=(uint16_t)(0x0120+i*4); s->copper[b+1]=(uint16_t)(a>>16);
        s->copper[b+2]=(uint16_t)(0x0122+i*4); s->copper[b+3]=(uint16_t)(a&0xFFFF);
    }
    /* layer-repos region (right after the sprite pointers, BEFORE the colour-reload
     * region so its early WAIT executes first): NOP-initialised, so it is inert until a
     * game calls hwscroll_layer_repos() each frame. */
    s_lrepos = total*4 + 2 + HWS_NSPR*4;
    for (int i=0;i<HWS_LREPOS_WORDS;i+=2){ s->copper[s_lrepos+i]=0x01FE; s->copper[s_lrepos+i+1]=0; }
    /* HUD band-split region (right after layer-repos): NOP-initialised, inert until a
     * single-playfield game calls hwscroll_hud_bands() each frame. */
    s_hudsplit = s_lrepos + HWS_LREPOS_WORDS;
    for (int i=0;i<HWS_HUDSPLIT_WORDS;i+=2){ s->copper[s_hudsplit+i]=0x01FE; s->copper[s_hudsplit+i+1]=0; }
    /* attached-pair colour-reload region starts right after the HUD-split region; seed
     * it with an immediate end marker (no reloads until hwscroll_aspr_finish runs). */
    s_relbase = s_hudsplit + HWS_HUDSPLIT_WORDS;
    s->copper[s_relbase+0]=0xFFFF; s->copper[s_relbase+1]=0xFFFE;
    /* sprite HSTART for screen x=0 = the display window's left DIW edge (so hw sprites
     * align with the playfield). disp_w<320 is centred -> same 0x81+(320-w)/2 as DIWSTRT. */
    s_spr_base_h = 0x81 + (320 - disp_w)/2 + hwscroll_hwin_adj;
    for (int i=0;i<HWS_ASPR_PAIRS;i++){ s_apair_cur[i]=0; s_apair_lasty[i]=-1000000; }
    s_aspr_n=0;
    { int g=0; while(g++<100000){ if((c[R_VPOSR]&0x1FF)>0x80) break; } }
    c[R_DMACON]=0x7FFF;
    /* Display window sized to disp_w (centred), so a narrow game (224) doesn't pay
     * for 320px of bitplane DMA -- that DMA is what starves the CPU's bob writes on
     * AGA. Reproduces the 320 values for disp_w=320; matches the C2P 224 window for
     * disp_w=224. (-8 on DDFSTRT keeps the one-extra-fetch-word for fine scroll.) */
    { int hstart=0x81+(320-disp_w)/2+hwscroll_hwin_adj, ddf=(hstart-0x11)/2;
      /* The no-preroll flag is set by vertical/flat scrollers so column 0 lands at
       * the left DIW edge.  Use explicit if/else to avoid an m68k gcc bug that
       * optimised the ternary to a constant. */
      /* Memory barrier so the compiler reloads the run-time value of the volatile
       * flag; without it gcc 6.5 optimises the test away and always uses preroll=8. */
      asm volatile ("" ::: "memory");
      int preroll = 8;
      if (hwscroll_no_preroll) preroll = 0;
      int rslack = preroll ? 8 : 0;
      /* vertical window 0x2C..0x12C (256 lines); opt-in symmetric crop tightens it
       * by hwscroll_vcrop_top at the top edge and hwscroll_vcrop_bot at the bottom. */
      int vstart = 0x2C + hwscroll_vcrop_top;          /* DIWSTRT V */
      int vstop  = 0x2C - hwscroll_vcrop_bot;          /* DIWSTOP V low byte (VSTOP=0x12C-bot) */
      c[R_DIWSTRT]=(uint16_t)(((vstart&0xff)<<8) | (hstart&0xff));
      c[R_DIWSTOP]=(uint16_t)(((vstop &0xff)<<8) | ((hstart+disp_w)&0xff));
      c[R_DDFSTRT]=(uint16_t)(ddf-preroll);
      c[R_DDFSTOP]=(uint16_t)(ddf + (disp_w/16-1)*8 + rslack); }
    c[R_BPLCON0]=bplcon0_for(s);
    c[R_BPLCON1]=0;
    /* FMODE: set the AGA sprite-fetch width (16/32/64px per HWS_SPR_W). Bitplane-fetch
     * bits (0,1) stay 0 (16-bit bpl fetch) -- only the sprite nibble changes. */
    c[R_FMODE]=HWS_FMODE_SPR;
    /* BPLCON2: PF1P=PF2P=4 (bits 2-0 and 5-3) so the playfields sit BEHIND the
     * sprites -- with 0 the (opaque) playfields cover all sprites. */
    c[R_BPLCON2]=0x0024;
    /* PF2 colour offset = 2^pfplanes (PF2 uses colours [2^pfplanes .. *2-1]); the
     * BPLCON3 PF2OF code (bits 12-10) for offset 2^n is n: 3->8 (3+3), 4->16 (4+4). */
    /* PF2 colour offset = 32 (PF2OF code 5): the back playfield uses COLOR32-47,
     * NOT 16-31 -- because hardware SPRITES live at 16-31. With the old offset 16
     * a 4+4 dual-pf put the ocean in 16-31 and the sprite palette overwrote it
     * (green sea). Layout now: PF1 0-15, sprites 16-31, PF2 32-47. */
    c[R_BPLCON3]=(nlayers==2)?(uint16_t)(5<<10):0;
    /* compensate the EXTRA fetch words: pre-roll (left, 2 bytes) + right slack (2
     * bytes) when fine scroll is in use => 4 bytes; 0 otherwise. */
    { int modadj = 4;
      asm volatile ("" ::: "memory");
      if (hwscroll_no_preroll) modadj = 0;
      c[R_BPL1MOD]=(uint16_t)(s->stride - disp_w/8 - modadj);
      c[R_BPL2MOD]=(uint16_t)(s->stride - disp_w/8 - modadj); }
    { uint32_t a=(uint32_t)s->copper; c[R_COP1LCH]=(uint16_t)(a>>16); c[R_COP1LCH+1]=(uint16_t)(a&0xFFFF); }
    c[R_DMACON]=0x83E0;   /* +SPREN (sprite DMA) */
    /* Disable OS interrupts so the system keyboard handler doesn't consume keycodes
     * before the game's CIA poll reads them (the C2P video path does the same --
     * without this, coin/start keys never register). */
    c[R_INTENA]=0x7FFF; c[R_INTREQ]=0x7FFF;
    if (hwscroll_intena_keep) c[R_INTENA]=hwscroll_intena_keep;   /* keep audio CIA int alive */
    Forbid();
    s->ok=1; return 1;
}

void hwscroll_palette(hwscroll_t *s, int layer, const uint16_t *rgb12, int n){
    volatile uint16_t *c=CUSTOM; int base=(layer==1)?32:0;   /* PF2 colour base (see open) */
    /* AGA-banked: colours 0-255 in banks of 32 via BPLCON3 bits 15-13. */
    for (int i=0;i<n;i++){ int idx=base+i; if(idx>255)break;
        c[R_BPLCON3]=(uint16_t)((idx>>5)<<13); c[R_COLOR00+(idx&31)]=rgb12[i]; }
    c[R_BPLCON3]=(s->nlayers==2)?(uint16_t)(5<<10):0;   /* restore PF2 offset/bank 0 */
}

/* AGA 8-bit-per-channel palette (24-bit) via LOCT: each colour is loaded in two
 * writes -- high nibbles (LOCT=0) then low nibbles (LOCT=1). rgb = 3 bytes/colour
 * (R,G,B 0-255). Needed for 1943's resistor-weighted palette, which 12-bit
 * (4-bit/channel) mangles into wrong hues. */
#define R_LOCT 0x200                              /* BPLCON3 bit 9 */
void hwscroll_palette8(hwscroll_t *s, int layer, const uint8_t *rgb, int n){
    volatile uint16_t *c=CUSTOM; int base=(layer==1)?32:0;
    for (int i=0;i<n;i++){ int idx=base+i; if(idx>255)break;
        uint16_t bank=(uint16_t)((idx>>5)<<13);
        int r=rgb[i*3], g=rgb[i*3+1], b=rgb[i*3+2];
        c[R_BPLCON3]=bank;                        /* LOCT=0 -> high nibbles */
        c[R_COLOR00+(idx&31)]=(uint16_t)(((r>>4)<<8)|((g>>4)<<4)|(b>>4));
        c[R_BPLCON3]=(uint16_t)(bank|R_LOCT);     /* LOCT=1 -> low nibbles  */
        c[R_COLOR00+(idx&31)]=(uint16_t)(((r&0xf)<<8)|((g&0xf)<<4)|(b&0xf));
    }
    c[R_BPLCON3]=(s->nlayers==2)?(uint16_t)(5<<10):0;   /* clear LOCT/bank, restore PF2 offset */
}

uint8_t *hwscroll_buffer(hwscroll_t *s, int layer, int plane){
    return s->bpl[layer] + (long)plane*s->plane_sz;
}

void hwscroll_putpix(hwscroll_t *s, int layer, int x, int y, uint8_t pen){
    if((unsigned)x>=(unsigned)s->buf_w || (unsigned)y>=(unsigned)s->buf_h) return;
    uint8_t *row=s->bpl[layer] + (long)y*s->stride + (x>>3); uint8_t m=0x80>>(x&7);
    for(int pl=0;pl<s->pfplanes;pl++){ if(pen&(1<<pl)) row[pl*s->plane_sz]|=m; else row[pl*s->plane_sz]&=(uint8_t)~m; }
}

void hwscroll_set(hwscroll_t *s, int layer, int x, int y){
    if(!s->ok) return;
    s_scrollx[layer]=x; s_scrolly[layer]=y; s_fine[layer]=(16-(x&15))&15;
    set_ptrs(s);
    /* BPLCON1: PF1 (layer 0) low nibble, PF2 (layer 1) high nibble. Written into the
     * COPPER fine-scroll MOVE (not the live register) so it is latched at frame top
     * together with the coarse pointers above -- no 16px-boundary judder. */
    int f0=s_fine[0], f1=(s->nlayers==2)?s_fine[1]:s_fine[0];
    if (s_bplcon1_slot>=0)
        s->copper[s_bplcon1_slot+1]=(uint16_t)((f0&15) | ((f1&15)<<4));
}

void hwscroll_set_split(hwscroll_t *s, int layer, int scroll, int y_top, int y_bot){
    if(!s->ok) return;
    s_scrollx[layer]=0; s_scrolly[layer]=0; s_fine[layer]=0;   /* frame-top = top HUD */
    set_ptrs(s);
    int f0=s_fine[0], f1=(s->nlayers==2)?s_fine[1]:s_fine[0];
    if (s_bplcon1_slot>=0)
        s->copper[s_bplcon1_slot+1]=(uint16_t)((f0&15) | ((f1&15)<<4));
    s_fg_split=1; s_fg_split_layer=layer; s_fg_split_scroll=scroll;
    s_fg_split_ytop=y_top; s_fg_split_ybot=y_bot;
}

/* Re-latch `layer`'s coarse pointers + fine scroll at a copper WAIT one line before the
 * first display line (the proven WAIT-gated latch the FG play band uses), instead of the
 * frame-top no-WAIT BPLCON1 slot. Cures the continuous whole-layer shimmer a hardware-
 * scrolled dual-pf BACK playfield shows on lenient emulation. The OTHER playfield's fine
 * nibble is preserved. Call once per frame AFTER hwscroll_set()/hwscroll_set_split() so
 * the other layer's fine value is final. Inert (region stays NOP) if never called. */
void hwscroll_layer_repos(hwscroll_t *s, int layer, int scroll){
    if(!s->ok || s_lrepos<0) return;
    int w=s_lrepos;
    int coarse=((scroll+15)>>4)*2, fine=(16-(scroll&15))&15;
    int rast=SPR_BASE_V - 1;                              /* just before display line 0 */
    /* WAIT (rast, hp=0x70): late-in-line like emit_fg_repos, so the pointer MOVEs land in
     * hblank and never corrupt an active fetch. rast<255 so no two-step compare needed. */
    s->copper[w++]=(uint16_t)((rast<<8)|0x00E1); s->copper[w++]=0xFFFE;
    for (int p=0;p<s->pfplanes;p++){
        uint32_t a=(uint32_t)(s->bpl[layer] + (long)p*s->plane_sz + coarse);
        int hp=(s->nlayers==2)?(p*2+layer):p, reg=0x00E0 + hp*4;
        s->copper[w++]=(uint16_t)reg;     s->copper[w++]=(uint16_t)(a>>16);
        s->copper[w++]=(uint16_t)(reg+2); s->copper[w++]=(uint16_t)(a&0xFFFF);
    }
    { int f0,f1;
      if(layer==0){ f0=fine; f1=(s->nlayers==2)?s_fine[1]:0; }
      else        { f0=s_fine[0]; f1=fine; }
      s->copper[w++]=0x0102; s->copper[w++]=(uint16_t)((f0&15)|((f1&15)<<4)); }
    /* the trailing reserved words remain NOP (set in open) -> copper falls through */
}

/* Emit one single-playfield band reload at copper word w: WAIT the line before display
 * line `dline` (late HP -> MOVEs land in hblank; two-step compare for rast>255), then
 * set all pfplanes' BPLxPT to (bpl + plane*plane_sz + off) and BPLCON1=0. Returns new w. */
static int hud_emit(hwscroll_t *s, int w, uint8_t *bpl, long plane_sz, long off, int dline){
    int rast=SPR_BASE_V + dline - 1;
    if (rast>255){
        s->copper[w++]=(uint16_t)((255<<8)|0x0001); s->copper[w++]=0xFF00;
        s->copper[w++]=(uint16_t)(((rast&0xFF)<<8)|0x00E1); s->copper[w++]=0xFFFE;
    } else {
        s->copper[w++]=(uint16_t)((rast<<8)|0x00E1); s->copper[w++]=0xFFFE;
    }
    for (int p=0;p<s->pfplanes;p++){
        uint32_t a=(uint32_t)(bpl + (long)p*plane_sz + off);
        int reg=0x00E0 + p*4;
        s->copper[w++]=(uint16_t)reg;     s->copper[w++]=(uint16_t)(a>>16);
        s->copper[w++]=(uint16_t)(reg+2); s->copper[w++]=(uint16_t)(a&0xFFFF);
    }
    s->copper[w++]=0x0102; s->copper[w++]=0;          /* BPLCON1=0 (vertical-only scroller) */
    return w;
}

static void hudsplit_nop(hwscroll_t *s){
    if (s_hudsplit < 0) return;
    for (int i=0;i<HWS_HUDSPLIT_WORDS;i+=2){
        s->copper[s_hudsplit+i]=0x01FE; s->copper[s_hudsplit+i+1]=0;
    }
}

static void hudsplit_apply(hwscroll_t *s){
    if(!s_hud_pending || !s->ok || s_hudsplit<0 || s->nlayers!=1) return;
    int vc=hwscroll_vcrop_top;
    int w=s_hudsplit;
    hudsplit_nop(s);
    /* TOP band: rewrite the frame-top bitplane pointers (set by hwscroll_set) to the
     * HUD buffer top (row vc -> cropped identically to the play band). */
    for (int p=0;p<s->pfplanes;p++){
        uint32_t a=(uint32_t)(s_hud_hud_bpl + (long)p*s_hud_plane_sz + (long)vc*s->stride);
        s->copper[p*4+1]=(uint16_t)(a>>16);
        s->copper[p*4+3]=(uint16_t)(a&0xFFFF);
    }
    /* Single top-HUD band: line play_top hands the rest of the display to the scrolling
     * background ring.  Applied only during vblank by hwscroll_frame(), so the copper
     * cannot switch to the next back buffer halfway through the visible frame. */
    (void)s_hud_play_bot;
    w=hud_emit(s, w, s_hud_bg_bpl, s->plane_sz,
               (long)(s_hud_bg_v+vc+s_hud_play_top)*s->stride, s_hud_play_top);
    (void)w;
}

/* Single-playfield HUD band split. The display long axis is carved into three bands:
 *   [0, play_top)        TOP HUD    -> reads hud_bpl row 0 (frame-top ptrs overridden)
 *   [play_top, play_bot) PLAY area  -> reads bg_bpl scrolled (line play_top == bg row bg_v+play_top)
 *   [play_bot, disp_h)   BOTTOM HUD -> reads hud_bpl row play_bot
 * The HUD bands are a STATIC dedicated bitplane buffer (drawn from the score charram
 * only when it changes), so the persistent score/HUD glyphs are no longer per-frame
 * bobs. Call AFTER hwscroll_set() (it overrides the frame-top bitplane pointers) and
 * once per frame (bg_bpl alternates with double-buffering). Single playfield only. */
void hwscroll_hud_bands(hwscroll_t *s, uint8_t *bg_bpl, uint8_t *hud_bpl,
                        long hud_plane_sz, int bg_v, int play_top, int play_bot){
    if(!s->ok || s_hudsplit<0 || s->nlayers!=1) return;
    s_hud_bg_bpl = bg_bpl;
    s_hud_hud_bpl = hud_bpl;
    s_hud_plane_sz = hud_plane_sz;
    s_hud_bg_v = bg_v;
    s_hud_play_top = play_top;
    s_hud_play_bot = play_bot;
    s_hud_pending = 1;
    /* play_top/play_bot are BUFFER-ROW boundaries (the fg tile-column edges, vcrop-
     * independent): display line (row-vc) shows buffer row `row`. The bg play band thus
     * shows the SAME world slice a full-screen scroll showed at those lines (vcrop
     * cancels), and the HUD bottom band shows the HUD glyphs drawn at those rows. Only
     * the TOP band's frame-top pointer carries vcrop, so it crops like the bg. */
    /* SINGLE top-HUD band: the score is one line at the TOP only (cols 29,30,31).
     * One reload at play_top hands the rest of the display (play_top..disp_h) to the
     * scrolling bg ring -- no bottom band (that produced a spurious duplicate score).
     * Display line play_top shows the SAME bg row a full-screen scroll showed there
     * (V+vcrop+play_top); +vc keeps it crop-aligned with the top band. */
}

void hwscroll_frame(hwscroll_t *s){
    volatile uint16_t *c=CUSTOM; if(!s->ok) return;
    volatile uint32_t *vp=(volatile uint32_t*)0xdff004; unsigned long g=0;
    for(;;){ uint32_t r=*vp; uint32_t v=(((r>>16)&1)<<8)|((r>>8)&0xff); if(v<300)break; if(++g>600000UL)break; }
    g=0;
    for(;;){ uint32_t r=*vp; uint32_t v=(((r>>16)&1)<<8)|((r>>8)&0xff); if(v>=300)break; if(++g>600000UL)break; }
    /* sprite double-buffer reverted: it didn't cure the flicker (cause isn't the single-
     * buffer race) and is unverified. Single-buffer s->spr = spr_buf[0] (SPRxPT fixed at
     * open), exactly as before; sprite flicker to be re-diagnosed separately. */
    s->spr = s->spr_buf[0];
    hudsplit_apply(s);
    { uint32_t a=(uint32_t)s->copper; c[R_COP1LCH]=(uint16_t)(a>>16); c[R_COP1LCH+1]=(uint16_t)(a&0xFFFF); }
    c[R_DMACON]=0x83E0;   /* +SPREN (sprite DMA) */
    c[R_INTENA]=0x7FFF;   /* re-disable OS interrupts each frame (keep the keyboard handler
                           * from eating coin/start keys -- audio init may have re-enabled them) */
    if (hwscroll_intena_keep) c[R_INTENA]=hwscroll_intena_keep;   /* but keep audio CIA int alive */
}

void hwscroll_frame_nowait(hwscroll_t *s){
    volatile uint16_t *c=CUSTOM; if(!s->ok) return;
    s->spr = s->spr_buf[0];
    hudsplit_apply(s);
    { uint32_t a=(uint32_t)s->copper; c[R_COP1LCH]=(uint16_t)(a>>16); c[R_COP1LCH+1]=(uint16_t)(a&0xFFFF); }
    c[R_DMACON]=0x83E0;
}

/* write one sprite's POS/CTL/data at blk; returns words written (SPR1). */
static int write_sprite(uint16_t *blk, int x, int y, const uint16_t *img){
    int vstart=SPR_BASE_V + y, vstop=vstart + HWS_SPR_H, hstart=SPR_BASE_H + x;
    blk[0]=(uint16_t)(((vstart&0xFF)<<8) | ((hstart>>1)&0xFF));            /* SPRxPOS */
    blk[HWS_SPR_WORDS]=(uint16_t)(((vstop&0xFF)<<8) | (((vstart>>8)&1)<<2)
                       | (((vstop>>8)&1)<<1) | (hstart&1));                /* SPRxCTL */
    for (int i=0;i<HWS_SPR_H*HWS_SPR_LINEW;i++) blk[HWS_SPR_CTLW+i]=img[i];
    return SPR1;
}

/* position sprite idx and load its image. visible=0 -> inactive (zeroed block). */
void hwscroll_sprite(hwscroll_t *s, int idx, int x, int y, const uint16_t *img, int visible){
    if(!s->ok || idx<0 || idx>=HWS_NSPR) return;
    uint16_t *blk = s->spr + idx*SPRBLK;
    if (!visible || !img){ blk[0]=0; blk[1]=0; return; }
    int w=write_sprite(blk, x, y, img);
    blk[w]=0; blk[w+1]=0;                                                  /* end */
}

/* ---- sprite multiplexing ---- */
void hwscroll_sprites_clear(hwscroll_t *s){
    for (int i=0;i<HWS_NSPR;i++){ s_chan_cur[i]=0; s_chan_lasty[i]=-1000; }
}
/* greedily place a sprite in the first channel free at screen-y; feed sorted by y. */
int hwscroll_sprites_add(hwscroll_t *s, int x, int y, const uint16_t *img){
    if(!s->ok) return 0;
    for (int ch=0; ch<HWS_NSPR; ch++){
        if (y < s_chan_lasty[ch]) continue;                  /* this channel still busy at y */
        if (s_chan_cur[ch] >= HWS_SPR_SLOTS*SPR1) continue;  /* channel full */
        uint16_t *blk = s->spr + ch*SPRBLK + s_chan_cur[ch];
        s_chan_cur[ch] += write_sprite(blk, x, y, img);
        s_chan_lasty[ch] = y + HWS_SPR_H + 1;                /* next sprite must start below */
        return 1;
    }
    return 0;                                                /* all channels busy here */
}
void hwscroll_sprites_finish(hwscroll_t *s){
    if(!s->ok) return;
    for (int ch=0; ch<HWS_NSPR; ch++){
        uint16_t *blk = s->spr + ch*SPRBLK + s_chan_cur[ch];
        blk[0]=0; blk[1]=0;                                  /* terminate the chain */
    }
}

void hwscroll_sprite_colour(hwscroll_t *s, int idx, int pen, uint16_t rgb12){
    volatile uint16_t *c=CUSTOM; (void)s;
    if(idx<0||idx>=HWS_NSPR||pen<1||pen>3) return;
    int reg = 16 + (idx>>1)*4 + pen;          /* sprites use COLOR16-31 (pair0->17-19, ...) */
    c[R_COLOR00+reg]=rgb12;
}

/* ================= ATTACHED-PAIR (15-colour, LOSSLESS) hardware sprites =================
 * Each on-screen sprite = TWO hw channels ATTACHED into one 4-bitplane (16-colour, pen 0
 * transparent) sprite. Pair p uses channels 2p (planes 0,1) + 2p+1 (planes 2,3, ATTACH
 * bit set). All 4 pairs composite out of COLOR16..31, so only ONE 15-colour palette can be
 * live on any scanline -- the COPPER reloads COLOR17..31 just above each band, and the
 * router (hwscroll_aspr_add) refuses to place a sprite whose vertical band overlaps an
 * already-placed sprite of a DIFFERENT palette. Each pair also MULTIPLEXES vertically. */

/* write one attached sprite into pair `pair`'s two channel chains at their shared cursor. */
static void write_aspr(hwscroll_t *s, int pair, int x, int y, const uint16_t *img4){
    int che=pair*2, cho=pair*2+1;
    uint16_t *be = s->spr + che*SPRBLK + s_apair_cur[pair];
    uint16_t *bo = s->spr + cho*SPRBLK + s_apair_cur[pair];
    int vstart=SPR_BASE_V + y, vstop=vstart + HWS_SPR_H, hstart=s_spr_base_h + x;
    uint16_t pos =(uint16_t)(((vstart&0xFF)<<8) | ((hstart>>1)&0xFF));
    uint16_t ctl =(uint16_t)(((vstop&0xFF)<<8) | (((vstart>>8)&1)<<2)
                              | (((vstop>>8)&1)<<1) | (hstart&1));
    be[0]=pos; be[HWS_SPR_WORDS]=ctl;                  /* even channel: planes 0,1          */
    bo[0]=pos; bo[HWS_SPR_WORDS]=(uint16_t)(ctl|0x0080);/* odd channel: planes 2,3 + ATTACH  */
    /* img4 = HWS_SPR_H lines, each = 4 planes x HWS_SPR_WORDS words, plane-major:
     * [plane0 w0..wN][plane1 ..][plane2 ..][plane3 ..]. Even ch gets planes 0,1 (DATA,DATB),
     * odd ch gets planes 2,3. (At W=16 this is the classic 4-words-per-line layout.) */
    for (int i=0;i<HWS_SPR_H;i++){
        const uint16_t *L = img4 + i*4*HWS_SPR_WORDS;
        int d = HWS_SPR_CTLW + i*HWS_SPR_LINEW;
        for (int w=0; w<HWS_SPR_WORDS; w++){
            be[d + w]                = L[0*HWS_SPR_WORDS + w];   /* plane0 -> even DATA */
            be[d + HWS_SPR_WORDS + w]= L[1*HWS_SPR_WORDS + w];   /* plane1 -> even DATB */
            bo[d + w]                = L[2*HWS_SPR_WORDS + w];   /* plane2 -> odd  DATA */
            bo[d + HWS_SPR_WORDS + w]= L[3*HWS_SPR_WORDS + w];   /* plane3 -> odd  DATB */
        }
    }
    s_apair_cur[pair] += SPR1;
}

void hwscroll_aspr_clear(hwscroll_t *s){
    (void)s;
    for (int p=0;p<HWS_ASPR_PAIRS;p++){ s_apair_cur[p]=0; s_apair_lasty[p]=-1000000; }
    s_aspr_n=0;
}

/* place (x,y) screen sprite if a pair is free at its band AND no overlapping band holds a
 * different palette; feed sorted by ASCENDING y. Returns 1 = hardware, 0 = caller bobs. */
void hwscroll_aspr_save(hwscroll_t *s, hwscroll_aspr_state_t *st){
    (void)s;
    for (int p=0;p<HWS_ASPR_PAIRS;p++){ st->cur[p]=s_apair_cur[p]; st->lasty[p]=s_apair_lasty[p]; }
    st->n=s_aspr_n;
}
void hwscroll_aspr_restore(hwscroll_t *s, const hwscroll_aspr_state_t *st){
    (void)s;
    for (int p=0;p<HWS_ASPR_PAIRS;p++){ s_apair_cur[p]=st->cur[p]; s_apair_lasty[p]=st->lasty[p]; }
    s_aspr_n=st->n;
}

int hwscroll_aspr_add(hwscroll_t *s, int x, int y, const uint16_t *img4,
                      int palid, const uint16_t *pal15){
    if(!s->ok) return 0;
    /* HARD CAP: never exceed the placed-sprite LOG capacity. Every hardware sprite
     * MUST be logged (so _finish can emit its copper palette reload); a sprite placed
     * without a log entry would show wrong colours AND, more importantly, an unlogged
     * write past s_aspr_*[] would corrupt adjacent statics. At the cap all 4 pairs are
     * already full anyway, so this just routes the overflow to the software fallback. */
    if (s_aspr_n >= HWS_ASPR_PAIRS*HWS_ASPR_SLOTS) return 0;
    /* the copper colour reload waits at vpos = SPR_BASE_V+y-2, which must fit the 8-bit
     * copper vertical compare (<=255). Sprites lower than that band go to a bob (avoids
     * the copper >255-line wrap entirely -- the device-safe choice). */
    if (SPR_BASE_V + y - 2 > 255) return 0;
    /* palette-conflict test against already-placed sprites whose band overlaps [y,y+H). */
    for (int k=0;k<s_aspr_n;k++){
        if (s_aspr_pid[k]==palid) continue;
        if (y < s_aspr_py[k]+HWS_SPR_H && s_aspr_py[k] < y+HWS_SPR_H) return 0;
    }
    /* first pair free at this y with chain room. */
    int pair=-1;
    for (int p=0;p<HWS_ASPR_PAIRS;p++){
        if (y < s_apair_lasty[p]) continue;
        if (s_apair_cur[p] + SPR1 > SPRBLK-HWS_SPR_CTLW) continue;  /* leave room for the end marker */
        pair=p; break;
    }
    if (pair<0) return 0;
    write_aspr(s, pair, x, y, img4);
    s_apair_lasty[pair] = y + HWS_SPR_H + 1;
    if (s_aspr_n < HWS_ASPR_PAIRS*HWS_ASPR_SLOTS){
        s_aspr_py[s_aspr_n]=y; s_aspr_pid[s_aspr_n]=palid;
        for (int i=0;i<15;i++) s_aspr_pal[s_aspr_n][i]=pal15[i];
        s_aspr_n++;
    }
    return 1;
}

/* terminate the 8 channel chains and (re)build the copper per-band colour reload list.
 * placed-log is ascending-y; emit a WAIT + 15 COLOR moves whenever the live palette must
 * change (no two overlapping bands differ, guaranteed by the add-time conflict check). */
/* Emit one PF1 scroll-split reposition at copper word w: WAIT the line before
 * display line `disp_line` (late hp -> MOVEs land in hblank), set this layer's
 * bitplane ptrs to (disp_line row + coarse scroll) + PF1 fine nibble (other pf
 * nibble kept). Two-step WAIT for raster>255. Returns new w. */
static int emit_fg_repos(hwscroll_t *s, int w, int disp_line, int scroll){
    int layer=s_fg_split_layer;
    int coarse=((scroll+15)>>4)*2, fine=(16-(scroll&15))&15;
    int bgfine=(s->nlayers==2)?s_fine[1]:0;
    long off=(long)disp_line*s->stride + coarse;
    int rast=SPR_BASE_V + disp_line - 1;
    if (rast>255){
        s->copper[w++]=(uint16_t)((255<<8)|0x0001); s->copper[w++]=0xFF00;
        s->copper[w++]=(uint16_t)(((rast&0xFF)<<8)|0x00E1); s->copper[w++]=0xFFFE;
    } else {
        s->copper[w++]=(uint16_t)((rast<<8)|0x00E1); s->copper[w++]=0xFFFE;
    }
    for (int p=0;p<s->pfplanes;p++){
        uint32_t a=(uint32_t)(s->bpl[layer] + (long)p*s->plane_sz + off);
        int hp=(s->nlayers==2)?(p*2+layer):p, reg=0x00E0 + hp*4;
        s->copper[w++]=(uint16_t)reg;     s->copper[w++]=(uint16_t)(a>>16);
        s->copper[w++]=(uint16_t)(reg+2); s->copper[w++]=(uint16_t)(a&0xFFFF);
    }
    s->copper[w++]=0x0102;
    s->copper[w++]=(uint16_t)((fine&15)|((bgfine&15)<<4));
    return w;
}

void hwscroll_aspr_finish(hwscroll_t *s){
    if(!s->ok) return;
    for (int p=0;p<HWS_ASPR_PAIRS;p++){
        uint16_t *be = s->spr + (p*2)*SPRBLK   + s_apair_cur[p];
        uint16_t *bo = s->spr + (p*2+1)*SPRBLK + s_apair_cur[p];
        be[0]=0; be[HWS_SPR_WORDS]=0; bo[0]=0; bo[HWS_SPR_WORDS]=0;  /* POS=CTL=0 end marker */
    }
    int w=s_relbase, lim=s_relbase + HWS_MAXREL*ASPR_RELWORDS + HWS_FGSPLIT_WORDS;
    int lastpid=0x7fffffff, fgtop_done=!s_fg_split;
    int ftr=SPR_BASE_V + s_fg_split_ytop;          /* play-band start raster (sort key) */
    for (int k=0;k<s_aspr_n;k++){
        if (s_aspr_pid[k]==lastpid) continue;          /* palette already live on this band */
        lastpid=s_aspr_pid[k];
        int vp=SPR_BASE_V + s_aspr_py[k] - 2; if(vp<0) vp=0; if(vp>255) continue;
        if (!fgtop_done && vp>=ftr && w+HWS_FGSPLIT_WORDS<=lim){
            w=emit_fg_repos(s,w,s_fg_split_ytop,s_fg_split_scroll); fgtop_done=1; }
        if (w + ASPR_RELWORDS > lim) break;            /* reload region full */
        s->copper[w++]=(uint16_t)((vp<<8) | 0x0001);   /* WAIT vpos, hp=0, bit0=1           */
        s->copper[w++]=0xFF00;                          /* VE=all-vertical, HE=ignore horiz  */
        for (int i=0;i<15;i++){                          /* COLOR17..COLOR31 = pens 1..15     */
            s->copper[w++]=(uint16_t)(0x0180 + (17+i)*2);
            s->copper[w++]=s_aspr_pal[k][i];
        }
    }
    if (s_fg_split && w+HWS_FGSPLIT_WORDS<=lim){
        if (!fgtop_done) w=emit_fg_repos(s,w,s_fg_split_ytop,s_fg_split_scroll);
        if (s_fg_split_ybot < 256) w=emit_fg_repos(s,w,s_fg_split_ybot,0);   /* bottom HUD -> static; SKIP when y_bot>=256 (single split: top rows static, row 5..bottom all scroll) */
    }
    s->copper[w++]=0xFFFF; s->copper[w++]=0xFFFE;       /* end of copper list */
}

int hwscroll_aspr_count(hwscroll_t *s){ (void)s; return s_aspr_n; }
