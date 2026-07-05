/* src/hal/shinobi_hwrender.c -- native AGA renderer for Shinobi (Sega System 16B).
 *
 * First-light renderer.  It is the on-target counterpart of the host-proven
 * tools/shinobi_shot.c: it ports that file's EXACT System-16B video logic
 * (2 scrolling tilemap layers + the fixed text layer + the 315-5196 sprite list,
 * with the xBGR555 palette) straight to an AGA 8-bitplane / 256-colour
 * playfield.  Because we composite in SOFTWARE (not via AGA hardware sprites)
 * the sprite hardware-ZOOM "just works" (the same per-pixel hzoom / per-row
 * vzoom accumulator math shinobi_shot.c uses) -- no hw-sprite budget, no
 * per-band palette reload, no palette-per-playfield split.  The whole 320x224
 * frame is built into a chunky pen buffer each frame and blitted to the planes.
 *
 * SELF-CONTAINED display: rather than depend on the shared src/hal/hwscroll.c
 * (which another agent is editing concurrently), this file carries its OWN
 * minimal single-playfield AGA bring-up (chip bitplanes + copper bitplane
 * pointers + the proven DIW/DDF/BPLCON setup copied from hwscroll.c's single-pf
 * path).  No hardware scroll is needed -- the renderer composites the already
 * scrolled image (the System-16 textram scroll latches are applied in software,
 * exactly as shinobi_shot.c does).
 *
 * Colour: System-16 has 2048 palette entries.  For speed, the RTG build maps
 * those RGB values directly to a fixed RGB332 256-colour screen palette, matching
 * the Capcom RTG ports' fast path and avoiding per-frame palette reduction.
 *
 * gfx is the BUILD-TIME decode (tools/shinobi_decode_gfx.py): tiles = the 3
 * gfx_8x8x3_planar mask ROMs (mpr-11363/64/65), sprites = the assembled 16-bit
 * BE region (build/shinobi/shinobi_spr.bin), both incbin'd by shinobi_gfxdata.s.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <graphics/gfxbase.h>
#include <stdint.h>
#include <string.h>
#include "shinobi_assets.h"

/* ====================================================================== */
/* MINIMAL SINGLE-PLAYFIELD AGA DISPLAY (copied from hwscroll.c single-pf). */
/* ====================================================================== */
#define SW       320
#define SH       224
#define NPLANES  8
#define STRIDE   (SW/8)              /* 40 bytes / bitplane row              */
#define PLANESZ  ((long)STRIDE*SH)   /* 8960                                 */

#define CUSTOM    ((volatile uint16_t *)0xdff000)
#define R_DMACON  (0x096/2)
#define R_DIWSTRT (0x08E/2)
#define R_DIWSTOP (0x090/2)
#define R_DDFSTRT (0x092/2)
#define R_DDFSTOP (0x094/2)
#define R_BPLCON0 (0x100/2)
#define R_BPLCON1 (0x102/2)
#define R_BPLCON2 (0x104/2)
#define R_BPLCON3 (0x106/2)
#define R_BPL1MOD (0x108/2)
#define R_BPL2MOD (0x10A/2)
#define R_COLOR00 (0x180/2)
#define R_COP1LCH (0x080/2)
#define R_VPOSR   (0x004/2)
#define R_INTENA  (0x09A/2)
#define R_INTREQ  (0x09C/2)
#define R_FMODE   (0x1FC/2)
#define R_LOCT    0x200             /* BPLCON3 bit 9 = LOCT (low-nibble bank) */

/* ---- COPPER-driven 256-colour AGA palette -----------------------------------
 * The palette MUST be loaded by the copper at the top of the frame, NOT by CPU
 * pokes inside shinobi_render().  The translator runs the guest natively and only
 * reaches shinobi_render() on a guest-frame boundary (rare: ~1 per many display
 * frames), at an unpredictable beam position; a CPU LOCT/bank upload there lands
 * mid-display and the emulator/Denise beam-replay scrambles the BPLCON3 bank/LOCT
 * <-> COLOR association, so only a corrupt partial subset survives in the base
 * palette -> the title showed as a grey ramp.  Instead we build the whole AGA
 * 8-bit-per-gun palette (8 banks x 32 pens, hi then lo nibble via LOCT) as copper
 * MOVEs at the START of the copper list; the copper re-runs it every frame at a
 * deterministic vblank beam position.  shinobi_render() then just pokes the colour
 * VALUE words into the (chip-RAM) copper list -- no beam timing required. */
#define PAL_BANKS       8
#define PAL_BANK_WORDS  132         /* per bank: BPLCON3 + 32 COLOR (hi) + BPLCON3 + 32 COLOR (lo) */
#define COP_PAL_WORDS   (PAL_BANKS*PAL_BANK_WORDS)   /* 1056 */
#define COP_RESTORE_IDX  COP_PAL_WORDS               /* "BPLCON3=0 for display" MOVE (2 words) */
#define COP_BPL_IDX     (COP_RESTORE_IDX+2)          /* 8 bitplane pointers (NPLANES*4 words)   */
#define COP_END_IDX     (COP_BPL_IDX + NPLANES*4)
#define COP_WORDS       (COP_END_IDX + 2)            /* + copper end                            */

struct GfxBase *GfxBase = 0;

static uint8_t  *s_planes;          /* NPLANES * PLANESZ, chip RAM           */
static uint16_t *s_copper;          /* copper list, chip RAM                 */
static int       s_ok;

/* poke one pen's 8-bit-per-gun colour into the copper list (hi + lo nibble words) */
static inline void cop_set_pen(int i, int r, int g, int b)
{
    int base = (i>>5)*PAL_BANK_WORDS, idx = i&31;
    s_copper[base + 3  + idx*2] = (uint16_t)(((r>>4)<<8)|((g>>4)<<4)|(b>>4));   /* hi nibbles */
    s_copper[base + 69 + idx*2] = (uint16_t)(((r&0xf)<<8)|((g&0xf)<<4)|(b&0xf)); /* lo nibbles */
}

static void disp_open(void)
{
    volatile uint16_t *c = CUSTOM;
    GfxBase = (struct GfxBase*)OpenLibrary((CONST_STRPTR)"graphics.library", 0);
    if (GfxBase){ LoadView(0); WaitTOF(); WaitTOF(); }

    long copsz = (long)COP_WORDS * 2;
    void *chunk = AllocMem((unsigned long)(NPLANES*PLANESZ) + copsz, MEMF_CHIP|MEMF_CLEAR);
    if (!chunk) return;
    s_planes = (uint8_t*)chunk;
    s_copper = (uint16_t*)(s_planes + NPLANES*PLANESZ);

    /* copper list: [256-colour AGA palette load] [restore BPLCON3] [8 bplptrs] [end].
     * The palette section is ~1056 MOVEs (~5 scanlines) and completes during the
     * top vblank, well before the display window opens at vpos 0x2C. Colour value
     * words start at 0 (black) and are filled by disp_palette8() each guest frame. */
    {
        int w = 0;
        for (int bk=0; bk<PAL_BANKS; bk++){
            s_copper[w++] = 0x0106; s_copper[w++] = (uint16_t)(bk<<13);          /* BPLCON3 bank, LOCT=0 */
            for (int idx=0; idx<32; idx++){ s_copper[w++] = (uint16_t)(0x0180+idx*2); s_copper[w++] = 0; }
            s_copper[w++] = 0x0106; s_copper[w++] = (uint16_t)((bk<<13)|R_LOCT); /* BPLCON3 bank, LOCT=1 */
            for (int idx=0; idx<32; idx++){ s_copper[w++] = (uint16_t)(0x0180+idx*2); s_copper[w++] = 0; }
        }
        s_copper[w++] = 0x0106; s_copper[w++] = 0;        /* BPLCON3=0: bank0/LOCT0 for the display */
        for (int hp=0; hp<NPLANES; hp++){
            uint32_t a = (uint32_t)(s_planes + hp*PLANESZ);
            s_copper[w++] = (uint16_t)(0x00E0 + hp*4); s_copper[w++] = (uint16_t)(a>>16);
            s_copper[w++] = (uint16_t)(0x00E2 + hp*4); s_copper[w++] = (uint16_t)(a&0xFFFF);
        }
        s_copper[w++] = 0xFFFF; s_copper[w++] = 0xFFFE;   /* copper end */
    }

    { int g=0; while (g++<100000){ if ((c[R_VPOSR]&0x1FF)>0x80) break; } }
    c[R_DMACON] = 0x7FFF;
    { int hstart = 0x81, ddf = (hstart-0x11)/2;          /* no h-scroll => no preroll */
      c[R_DIWSTRT] = (uint16_t)(0x2C00 | (hstart&0xff));
      c[R_DIWSTOP] = (uint16_t)(0x2C00 | ((hstart+SW)&0xff));
      c[R_DDFSTRT] = (uint16_t)ddf;
      c[R_DDFSTOP] = (uint16_t)(ddf + (SW/16-1)*8); }
    c[R_BPLCON0] = 0x0211;        /* 8 bitplanes, COLOR on (same as hwscroll planes>=8) */
    c[R_BPLCON1] = 0;
    c[R_FMODE]   = 0;
    c[R_BPLCON2] = 0x0024;
    c[R_BPLCON3] = 0;
    c[R_BPL1MOD] = (uint16_t)(STRIDE - SW/8);   /* 0 */
    c[R_BPL2MOD] = (uint16_t)(STRIDE - SW/8);
    { uint32_t a=(uint32_t)s_copper; c[R_COP1LCH]=(uint16_t)(a>>16); c[R_COP1LCH+1]=(uint16_t)(a&0xFFFF); }
    c[R_DMACON] = 0x8380;         /* SET|DMAEN|BPLEN|COPEN (no sprite DMA) */
    c[R_INTENA] = 0x7FFF; c[R_INTREQ] = 0x7FFF;
    Forbid();
    s_ok = 1;
}

/* Update all 256 pens in the copper palette list (8-bit-per-gun, hi+lo via LOCT).
 * rgb = 3 bytes/colour (R,G,B 0-255); pens >= n are forced black so a shrinking
 * palette never leaves stale colours behind.  Writes hit chip RAM (the copper
 * list), so the copper applies them at the next top-of-frame -- no beam timing. */
static void disp_palette8(const uint8_t *rgb, int n)
{
    for (int i=0;i<256;i++){
        if (i<n) cop_set_pen(i, rgb[i*3], rgb[i*3+1], rgb[i*3+2]);
        else     cop_set_pen(i, 0, 0, 0);
    }
}

/* once-per-frame: wait vblank, re-assert copper + DMA */
static void disp_frame(void)
{
    volatile uint16_t *c = CUSTOM; if (!s_ok) return;
    volatile uint32_t *vp = (volatile uint32_t*)0xdff004; unsigned long g=0;
    for(;;){ uint32_t r=*vp; uint32_t v=(((r>>16)&1)<<8)|((r>>8)&0xff); if(v<300)break; if(++g>600000UL)break; }
    g=0;
    for(;;){ uint32_t r=*vp; uint32_t v=(((r>>16)&1)<<8)|((r>>8)&0xff); if(v>=300)break; if(++g>600000UL)break; }
    { uint32_t a=(uint32_t)s_copper; c[R_COP1LCH]=(uint16_t)(a>>16); c[R_COP1LCH+1]=(uint16_t)(a&0xFFFF); }
    c[R_DMACON] = 0x8380;
    c[R_INTENA] = 0x7FFF;
}

/* ====================================================================== */
/* SHINOBI VIDEO (ported from tools/shinobi_shot.c, faithful to segas16b_v).  */
/* ====================================================================== */
static const uint8_t *G;            /* guest_base                            */
static int s_screen_flip;

/* guest VRAM word accessors (big-endian, as the 68000 bus stores them) */
static inline uint16_t tilew(unsigned off){ unsigned a=0x400000u+off; return (uint16_t)((G[a]<<8)|G[a+1]); }
static inline uint16_t textw(unsigned off){ unsigned a=0x410000u+off; return (uint16_t)((G[a]<<8)|G[a+1]); }
static inline uint16_t palw (unsigned idx){ unsigned a=0x840000u+idx*2; return (uint16_t)((G[a]<<8)|G[a+1]); }
static inline uint16_t sprw (unsigned wi){  return (uint16_t)((shinobi_gfx_spr[wi*2]<<8)|shinobi_gfx_spr[wi*2+1]); }

#define NTILES (0x40000/8)          /* 32768 -- Shadow Dancer: 3x 0x40000 tile planes */
static uint8_t *s_tilepix;          /* decoded tile pens: NTILES * 64 */

static void decode_tiles_once(void)
{
    if (s_tilepix) return;
    s_tilepix = (uint8_t*)AllocMem((unsigned long)NTILES * 64, MEMF_FAST|MEMF_CLEAR);
    if (!s_tilepix) s_tilepix = (uint8_t*)AllocMem((unsigned long)NTILES * 64, MEMF_PUBLIC|MEMF_CLEAR);
    if (!s_tilepix) return;
    for (int code=0; code<NTILES; code++) {
        uint8_t *dst = s_tilepix + (unsigned)code * 64;
        for (int y=0; y<8; y++) {
            unsigned b=(unsigned)code*8+y;
            unsigned p0=shinobi_gfx_tp0[b], p1=shinobi_gfx_tp1[b], p2=shinobi_gfx_tp2[b];
            for (int x=0; x<8; x++) {
                int sh=7-x;
                dst[y*8+x] = (uint8_t)((((p2>>sh)&1)<<2)|(((p1>>sh)&1)<<1)|((p0>>sh)&1));
            }
        }
    }
}

static inline int tile_pix(int code,int x,int y){
    if(code<0||code>=NTILES)return 0;
    if (s_tilepix) return s_tilepix[(unsigned)code*64 + y*8 + x];
    unsigned b=(unsigned)code*8+y; int sh=7-x;
    return (((shinobi_gfx_tp2[b]>>sh)&1)<<2)|(((shinobi_gfx_tp1[b]>>sh)&1)<<1)|((shinobi_gfx_tp0[b]>>sh)&1);
}

/* palette word -> 8-bit-per-gun RGB (xBGR555 + shade) */
static void pal_rgb(int idx,int *r,int *g,int *b){
    unsigned w = palw(idx);
    int r5=((w>>12)&0x01)|((w<<1)&0x1e);
    int g5=((w>>13)&0x01)|((w>>3)&0x1e);
    int b5=((w>>14)&0x01)|((w>>7)&0x1e);
    *r=(r5*255+15)/31; *g=(g5*255+15)/31; *b=(b5*255+15)/31;
}

/* ---- reduced 256-pen palette: index(0..2047) -> pen(0..255) ---- */
static uint8_t  s_pal256[256*3];
static uint8_t  s_idx2pen[2048];
static int      s_npens;
static int      s_pal_dirty = 1;
static int      s_fixed_palette_ready;

#define PAGE_W 512
#define PAGE_H 256
#define PAGE_PIXELS (PAGE_W * PAGE_H)
#define TILEMAP_PAGES 16
static uint8_t *s_pagepix;
static uint8_t *s_pagemask;
static uint16_t s_page_dirty = 0xffffu;
/* System-18 (ROM_BOARD_171_SHADOW) uses 8 tile sub-banks of 0x400 tiles each
 * (segaic16 banksize = 0x2000/8 = 0x400). Default = identity mapping, matching
 * MAME's tilemap_init (bank[i] = i) until the game programs port H. */
static uint8_t s_tile_bank[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

/* Host render-harness diagnostics (tools/shdancer_render.c). Purely additive:
 * these counters do not affect any rendering behaviour. */
static unsigned s_diag_tb8_calls = 0;
static uint8_t  s_diag_tb8_last  = 0;

static uint8_t rgb332(unsigned r, unsigned g, unsigned b)
{
    return (uint8_t)(((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6));
}

static void build_fixed_rgb332_palette(void)
{
    if (s_fixed_palette_ready) return;
    for (int i=0; i<256; i++) {
        unsigned r = ((unsigned)(i >> 5) * 255u) / 7u;
        unsigned g = ((unsigned)((i >> 2) & 7) * 255u) / 7u;
        unsigned b = ((unsigned)(i & 3) * 255u) / 3u;
        s_pal256[i*3+0] = (uint8_t)r;
        s_pal256[i*3+1] = (uint8_t)g;
        s_pal256[i*3+2] = (uint8_t)b;
    }
    s_npens = 256;
    s_fixed_palette_ready = 1;
}

static void build_palette(void){
    build_fixed_rgb332_palette();
    if (!s_pal_dirty)
        return;
    s_pal_dirty = 0;
    for (int idx=0; idx<2048; idx++){
        int r,g,b; pal_rgb(idx,&r,&g,&b);
        s_idx2pen[idx]=rgb332((unsigned)r, (unsigned)g, (unsigned)b);
    }
}

void shinobi_mark_palette_dirty(void)
{
    s_pal_dirty = 1;
    s_page_dirty = 0xffffu;
}

void shinobi_mark_tile_dirty(unsigned addr)
{
    unsigned off = (addr - 0x400000u) & 0xffffu;
    s_page_dirty |= (uint16_t)(1u << ((off >> 12) & 15));
}

/* Legacy 2-bank (System-16B) entry point.  System-18 banking is driven entirely
 * by shinobi_set_tile_bank8() (port H), so this is now a harmless no-op kept only
 * so the interpreter's existing save-state/init callers still link. */
void shinobi_set_tile_bank(int which, int bank)
{
    (void)which;
    (void)bank;
}

/* System-18 tile banking (segas18 rom_5874_bank_w): one port-H byte sets all 8
 * sub-banks -- low nibble drives banks 0..3, high nibble drives banks 4..7,
 * each spaced *4 with +i so a nibble selects a contiguous 0x1000-tile group. */
void shinobi_set_tile_bank8(uint8_t data)
{
    s_diag_tb8_calls++;
    s_diag_tb8_last = data;
    int changed = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t lo = (uint8_t)((data & 0x0f) * 4 + i);
        uint8_t hi = (uint8_t)(((data >> 4) & 0x0f) * 4 + i);
        if (s_tile_bank[0 + i] != lo) { s_tile_bank[0 + i] = lo; changed = 1; }
        if (s_tile_bank[4 + i] != hi) { s_tile_bank[4 + i] = hi; changed = 1; }
    }
    if (changed)
        s_page_dirty = 0xffffu;
}

static inline int tile_banked_code(int code)
{
    /* 13-bit tile code -> 8 sub-banks of 0x400 (banksize = 0x2000/8).  Bank
     * values run 0..0x3f, so the mapped index can reach 0xffff; wrap into the
     * decoded tile set (NTILES is a power of two) so it never runs off s_tilepix. */
    int t = (int)s_tile_bank[(code >> 10) & 7] * 0x400 + (code & 0x3ff);
    return t & (NTILES - 1);
}

/* ---- chunky pen frame ---- */
static uint8_t s_native[SH][SW];

void shinobi_set_screen_flip(int flip)
{
    s_screen_flip = flip ? 1 : 0;
}

static inline void put_native(int x, int y, uint8_t pen)
{
    if ((unsigned)x >= SW || (unsigned)y >= SH) return;
    if (s_screen_flip) {
        x = SW - 1 - x;
        y = SH - 1 - y;
    }
    s_native[y][x] = pen;
}

static inline void layer_params_for_row(int which, int sy, uint16_t *pages_out,
                                        int *xscroll_out, int *yscroll_out,
                                        int *colscroll_out)
{
    uint16_t pages = textw(0xe80 + which * 2);
    uint16_t yscroll = textw(0xe90 + which * 2);
    uint16_t xscroll = textw(0xe98 + which * 2);
    int colscroll = (yscroll & 0x8000) ? 1 : 0;
    int rowscrollindex = (s_screen_flip ? (216 - sy) : sy) / 8;

    if (rowscrollindex < 0) rowscrollindex = 0;
    if (rowscrollindex > 27) rowscrollindex = 27;

    uint16_t rowscroll = textw(0xf80 + 0x40 * which + rowscrollindex * 2);

    if (xscroll & 0x8000)
        xscroll = rowscroll;

    if (rowscroll & 0x8000) {
        int alt = which + 2;
        pages = textw(0xe80 + alt * 2);
        yscroll = textw(0xe90 + alt * 2);
        xscroll = textw(0xe98 + alt * 2);
        colscroll = 0;
    }

    *pages_out = pages;
    *xscroll_out = (0xc0 - xscroll) & 0x3ff;
    *yscroll_out = yscroll & 0x1ff;
    *colscroll_out = colscroll;
}

static inline int layer_col_yscroll(int which, int sx)
{
    int col = (sx + 8) >> 4;
    if (col < 0) col = 0;
    if (col > 27) col = 27;
    return textw(0xf16 + 0x40 * which + col * 2) & 0x1ff;
}

static inline void draw_tilemap_span(int sx, int end, int sy, uint16_t pages,
                                     int xscroll, int yscroll, int opaque)
{
    while (sx < end) {
        int vx=(sx+xscroll)&0x3ff, vy=(sy+yscroll)&0x1ff;
        int run = 8 - (vx & 7);
        if (run > end - sx) run = end - sx;
        int shift=((vx>=512)?4:0)+((vy>=256)?8:0);
        int page=(pages>>shift)&0xf;
        int pcol=(vx&511)>>3, prow=(vy&255)>>3;
        int tindex=prow*64+pcol;
        uint16_t w=tilew(page*0x1000 + tindex*2);
        int color=(w>>6)&0x7f, code=tile_banked_code(w&0x1fff);
        uint8_t *dst = &s_idx2pen[(color*8)&0x7ff];
        int py = vy & 7;
        for (int i=0; i<run; i++) {
            int pen=tile_pix(code, (vx+i)&7, py);
            if(opaque || pen) put_native(sx+i, sy, dst[pen]);
        }
        sx += run;
    }
}

static int ensure_page_cache(void)
{
    unsigned long bytes = (unsigned long)TILEMAP_PAGES * PAGE_PIXELS;
    if (s_pagepix && s_pagemask)
        return 1;
    if (!s_pagepix)
        s_pagepix = (uint8_t*)AllocMem(bytes, MEMF_FAST | MEMF_CLEAR);
    if (!s_pagepix)
        s_pagepix = (uint8_t*)AllocMem(bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (!s_pagemask)
        s_pagemask = (uint8_t*)AllocMem(bytes, MEMF_FAST | MEMF_CLEAR);
    if (!s_pagemask)
        s_pagemask = (uint8_t*)AllocMem(bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (!s_pagepix || !s_pagemask) {
        if (s_pagepix) { FreeMem(s_pagepix, bytes); s_pagepix = 0; }
        if (s_pagemask) { FreeMem(s_pagemask, bytes); s_pagemask = 0; }
        return 0;
    }
    s_page_dirty = 0xffffu;
    return 1;
}

static void rebuild_page_cache(int page)
{
    uint8_t *pix = s_pagepix + (unsigned long)page * PAGE_PIXELS;
    uint8_t *msk = s_pagemask + (unsigned long)page * PAGE_PIXELS;
    for (int ty = 0; ty < 32; ty++) {
        for (int tx = 0; tx < 64; tx++) {
            uint16_t w = tilew((unsigned)page * 0x1000u + (unsigned)(ty * 64 + tx) * 2u);
            int color = (w >> 6) & 0x7f;
            int code = tile_banked_code(w & 0x1fff);
            uint8_t *dst = &s_idx2pen[(color * 8) & 0x7ff];
            for (int py = 0; py < 8; py++) {
                unsigned base = (unsigned)(ty * 8 + py) * PAGE_W + (unsigned)tx * 8u;
                for (int px = 0; px < 8; px++) {
                    int pen = tile_pix(code, px, py);
                    pix[base + px] = dst[pen];
                    msk[base + px] = pen ? 1 : 0;
                }
            }
        }
    }
    s_page_dirty &= (uint16_t)~(1u << page);
}

static inline void ensure_cached_page_clean(int page)
{
    if (s_page_dirty & (1u << page))
        rebuild_page_cache(page);
}

static inline void draw_cached_tilemap_span(int sx, int end, int sy, uint16_t pages,
                                            int xscroll, int yscroll, int opaque)
{
    while (sx < end) {
        int vx = (sx + xscroll) & 0x3ff;
        int vy = (sy + yscroll) & 0x1ff;
        int run = PAGE_W - (vx & (PAGE_W - 1));
        if (run > end - sx) run = end - sx;
        int shift = ((vx >= PAGE_W) ? 4 : 0) + ((vy >= PAGE_H) ? 8 : 0);
        int page = (pages >> shift) & 0xf;
        unsigned off = (unsigned)(vy & (PAGE_H - 1)) * PAGE_W + (unsigned)(vx & (PAGE_W - 1));
        ensure_cached_page_clean(page);

        const uint8_t *src = s_pagepix + (unsigned long)page * PAGE_PIXELS + off;
        const uint8_t *msk = s_pagemask + (unsigned long)page * PAGE_PIXELS + off;
        if (!s_screen_flip) {
            uint8_t *dst = &s_native[sy][sx];
            if (opaque) {
                memcpy(dst, src, (size_t)run);
            } else {
                for (int i = 0; i < run; i++)
                    if (msk[i]) dst[i] = src[i];
            }
        } else {
            for (int i = 0; i < run; i++)
                if (opaque || msk[i])
                    put_native(sx + i, sy, src[i]);
        }
        sx += run;
    }
}

/* draw one tilemap layer (which: 0=foreground pen0-trans, 1=background opaque) */
static void draw_tilemap(int which,int opaque){
    int cached = ensure_page_cache();
    for (int sy=0; sy<SH; sy++){
        uint16_t pages;
        int xscroll, yscroll, colscroll;
        layer_params_for_row(which, sy, &pages, &xscroll, &yscroll, &colscroll);
        if (!colscroll) {
            if (cached)
                draw_cached_tilemap_span(0, SW, sy, pages, xscroll, yscroll, opaque);
            else
                draw_tilemap_span(0, SW, sy, pages, xscroll, yscroll, opaque);
        } else {
            for (int sx=0; sx<SW; ) {
                int local_yscroll = layer_col_yscroll(which, sx);
                int next = (((sx + 8) >> 4) + 1) * 16 - 8;
                if (next <= sx) next = sx + 16;
                if (next > SW) next = SW;
                if (cached)
                    draw_cached_tilemap_span(sx, next, sy, pages, xscroll, local_yscroll, opaque);
                else
                    draw_tilemap_span(sx, next, sy, pages, xscroll, local_yscroll, opaque);
                sx = next;
            }
        }
    }
}

/* draw text layer (64x28, scrolldx -192 => source x = screen x + 192) */
static void draw_text(void){
    for (int sy=0; sy<SH; sy++){
        int row=sy>>3; if(row>=28)continue;
        for (int sx=0; sx<SW; sx++){
            int tx=sx+192;
            int col=(tx>>3)&63;
            int tindex=row*64+col;
            uint16_t w=textw(tindex*2);
            /* text layer: tilemap_16b_text_info -> bank[0]*banksize + (data&0x1ff),
             * banksize = 0x400 for the System-18 8-bank scheme. */
            int color=(w>>9)&7, code=((int)s_tile_bank[0] * 0x400 + (w & 0x1ff)) & (NTILES - 1);
            int pen=tile_pix(code, tx&7, sy&7);
            if(pen==0)continue;
            put_native(sx, sy, s_idx2pen[(color*8+pen)&0x7ff]);
        }
    }
}

/* draw the 315-5196 sprite list (faithful to sega16sp.cpp; software, with zoom) */
#define SPRPAL_BASE 0x400
/* Shadow Dancer sprite region is 0x200000. The sega_sys16b_sprite_device (used
 * by both System 16B and shdancer's System 18 board) computes
 *   numbanks = region_bytes / 0x20000  and  spritedata = base + 0x10000*bank
 * (see MAME sega16sp.cpp sega_sys16b_sprite_device::draw), so 0x200000/0x20000
 * = 16 banks. (The "8" figure conflates the 0x40000 ROM size with the 0x20000
 * hardware bank unit; the device unit is 0x20000, matching sbase=0x10000*bank
 * below.) */
#define SPR_NUMBANKS (0x200000/0x20000)  /* 16 sprite banks (0x20000 bytes each) */
static inline void plot_spr(int x,int y,int colpri){
    int pix=colpri&0xf;
    if(pix==0||pix==15)return;
    if((unsigned)x>=SW||(unsigned)y>=SH)return;
    int idx = SPRPAL_BASE | (colpri & 0x3ff);
    put_native(x, y, s_idx2pen[idx&0x7ff]);
}
static void draw_sprites(void){
    const int ORIGINX=189;
    uint16_t sdata[8];
    for (int e=0;e<0x800/16;e++){
        const uint8_t *d=G+0x440000u+e*16;
        for (int i=0;i<8;i++)sdata[i]=(uint16_t)((d[i*2]<<8)|d[i*2+1]);
        if(sdata[2]&0x8000)break;
        int bottom=sdata[0]>>8, top=sdata[0]&0xff;
        int xpos=sdata[1]&0x1ff;
        int hide=sdata[2]&0x4000;
        int flip=sdata[2]&0x100;
        int pitch=(int8_t)(sdata[2]&0xff);
        unsigned addr=sdata[3];
        int bank=(sdata[4]>>8)&0xf;
        int colpri=((sdata[4]&0xff)<<4)|(((sdata[1]>>9)&0xf)<<12);
        int vzoom=(sdata[5]>>5)&0x1f, hzoom=sdata[5]&0x1f;
        if(hide||top>=bottom)continue;
        bank%=SPR_NUMBANKS;
        unsigned sbase=0x10000u*bank;
        int sx0=xpos-ORIGINX;
        unsigned yacc=0;
        for (int y=top;y<bottom;y++){
            addr+=pitch;
            yacc+=(unsigned)vzoom<<10;
            if(yacc&0x8000){addr+=pitch;yacc&=~0x8000u;}
            int scry=y;
            if((unsigned)scry>=SH){ continue; }
            int xacc=4*hzoom;
            unsigned a=addr;
            if(!flip){
                int x=sx0;
                for(;;){
                    unsigned pixels=sprw(sbase + (a&0xffff)); a++;
                    int pix;
                    pix=(pixels>>12)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr(x,scry,colpri|pix);x++;}
                    pix=(pixels>> 8)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr(x,scry,colpri|pix);x++;}
                    pix=(pixels>> 4)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr(x,scry,colpri|pix);x++;}
                    pix=(pixels>> 0)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr(x,scry,colpri|pix);x++;}
                    if(pix==15)break;
                    if(x>sx0+0x200)break;
                }
            }else{
                int x=sx0;
                for(;;){
                    unsigned pixels=sprw(sbase + (a&0xffff)); a--;
                    int pix;
                    pix=(pixels>> 0)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr(x,scry,colpri|pix);x++;}
                    pix=(pixels>> 4)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr(x,scry,colpri|pix);x++;}
                    pix=(pixels>> 8)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr(x,scry,colpri|pix);x++;}
                    pix=(pixels>>12)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr(x,scry,colpri|pix);x++;}
                    if(pix==15)break;
                    if(x>sx0+0x200)break;
                }
            }
        }
    }
}

/* chunky pen frame -> 8 bitplanes */
static void blit_native(void){
    for (int y=0; y<SH; y++){
        const uint8_t *nr = s_native[y];
        long rowoff = (long)y*STRIDE;
        for (int xb=0; xb<STRIDE; xb++){
            const uint8_t *p = nr + xb*8;
            int p0=p[0],p1=p[1],p2=p[2],p3=p[3],p4=p[4],p5=p[5],p6=p[6],p7=p[7];
            for (int pl=0; pl<NPLANES; pl++){
                uint8_t bv =
                    (uint8_t)((((p0>>pl)&1)<<7)|(((p1>>pl)&1)<<6)|(((p2>>pl)&1)<<5)|(((p3>>pl)&1)<<4)|
                              (((p4>>pl)&1)<<3)|(((p5>>pl)&1)<<2)|(((p6>>pl)&1)<<1)|(((p7>>pl)&1)));
                s_planes[pl*PLANESZ + rowoff + xb] = bv;
            }
        }
    }
}

/* ====================================================================== */
/* PUBLIC ENTRY POINTS                                                      */
/* ====================================================================== */
void shinobi_hw_open(void)
{
#ifdef SHINOBI_RTG
    /* RTG builds only need the software compositing buffers below; the presenter
     * owns the screen. */
    s_ok = 1;
#else
    disp_open();
#endif
}   /* call from hal_game_init (USER mode) */

void shinobi_hw_close(void)
{
    if (s_tilepix) {
        FreeMem(s_tilepix, (unsigned long)NTILES * 64);
        s_tilepix = 0;
    }
    if (s_pagepix) {
        FreeMem(s_pagepix, (unsigned long)TILEMAP_PAGES * PAGE_PIXELS);
        s_pagepix = 0;
    }
    if (s_pagemask) {
        FreeMem(s_pagemask, (unsigned long)TILEMAP_PAGES * PAGE_PIXELS);
        s_pagemask = 0;
    }
}

/* Strong override of the weak shinobi_render() in shinobi_dyntrans_amiga.c.
 * Called once per emulated frame (supervisor), after the guest updates VRAM. */
void shinobi_render(uint8_t *gbase)
{
    if (!s_ok) return;
    G = gbase;
    decode_tiles_once();
    build_palette();
    draw_tilemap(1,1);     /* background/parallax, opaque        */
    draw_tilemap(0,0);     /* foreground, pen0 transparent       */
    draw_text();           /* text layer, pen0 transparent       */
    draw_sprites();        /* sprites on top                     */
#ifndef SHINOBI_RTG
    disp_palette8(s_pal256, s_npens);
    blit_native();
    disp_frame();
#endif
    /* RTG build: stop at the chunky pen frame (s_native) + reduced palette (s_pal256);
     * the RTG main (shinobi_rtg_main.c) WriteChunkyPixels's it to an 8-bit screen. */
}

/* ---- RTG accessors: the chunky pen frame + the reduced 256-pen palette ---- */
const unsigned char *shinobi_chunky(void)        { return &s_native[0][0]; }
const unsigned char *shinobi_pal256(int *npens)  { if (npens) *npens = s_npens; return s_pal256; }
void shinobi_dims(int *w, int *h)                { if (w) *w = SW; if (h) *h = SH; }

/* ---- host render-harness diagnostics ----
 * calls          = number of System-18 port-H tile-bank writes (shinobi_set_tile_bank8)
 * last           = last data byte written to port H
 * banks_out[8]   = the resulting 8 tile sub-bank values
 * sprite_entries = active sprite-list entries before the 0x8000 terminator
 *                  (-1 if no frame has been rendered yet, so G is still 0). */
void shinobi_diag_get(unsigned *calls, unsigned *last,
                      unsigned char banks_out[8], int *sprite_entries)
{
    if (calls) *calls = s_diag_tb8_calls;
    if (last)  *last  = s_diag_tb8_last;
    if (banks_out)
        for (int i = 0; i < 8; i++) banks_out[i] = s_tile_bank[i];
    if (sprite_entries) {
        int n = -1;
        if (G) {
            n = 0;
            for (int e = 0; e < 0x800 / 16; e++) {
                const uint8_t *d = G + 0x440000u + (unsigned)e * 16u;
                uint16_t s2 = (uint16_t)((d[4] << 8) | d[5]);
                if (s2 & 0x8000) break;   /* sprite-list terminator (segas16 sprite dev) */
                n++;
            }
        }
        *sprite_entries = n;
    }
}
