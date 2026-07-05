/* src/hal/hwscroll.h -- generic Amiga AGA hardware-scrolled bitplane engine.
 *
 * Reusable scroll engine for native (non-C2P) renderers. Supports 1 layer (single
 * playfield, up to 8 planes) or 2 layers (dual-playfield parallax, N+N planes,
 * each scrolls independently). Owns the chip-RAM wide buffers, copper, and smooth
 * hardware scroll (per-pixel fine BPLCON1 + per-word coarse pointers + line-coarse
 * vertical), with the one-extra-fetch-word setup and matched fine/coarse phase for
 * jitter-free motion. A game draws its world into each layer's buffer and calls
 * hwscroll_set() per layer -- a few register writes per frame, no C2P.
 *
 * Proven on A1200/Amiberry: single-layer smooth scroll + dual-playfield parallax.
 * Next: blitter-bob sprites on top.
 */
#ifndef HWSCROLL_H
#define HWSCROLL_H
#include <stdint.h>

#define HWS_MAXLAYERS 2

#define HWS_NSPR     8              /* Amiga hardware sprites                    */
#define HWS_SPR_H    16             /* sprite image height (lines) in this engine */

typedef struct {
    uint8_t  *bpl[HWS_MAXLAYERS];   /* per-layer planar buffer (chip)            */
    uint16_t *copper;
    uint16_t *spr;                  /* 8 hardware-sprite data blocks (chip) = back buffer */
    uint16_t *spr_buf[2];           /* double-buffered sprite-data regions       */
    int   spr_back;                 /* index of back buffer (== spr)             */
    int   nlayers;                  /* 1 = single pf, 2 = dual-pf parallax        */
    int   pfplanes;                 /* planes PER layer (single: total; dual: each)*/
    int   disp_w, disp_h;
    int   buf_w, buf_h;
    int   stride;                   /* bytes per bitplane row                     */
    long  plane_sz;
    int   ok;
} hwscroll_t;

/* Open. nlayers 1 or 2; pfplanes = planes per layer (single-pf: total planes 1-8;
 * dual-pf: per-playfield, 2*pfplanes <= 8). Returns 1 on success. Takes over the
 * chipset (Forbid). Dual-pf: layer 0 = PF1 (front), layer 1 = PF2 (back). */
int  hwscroll_open(hwscroll_t *s, int nlayers, int pfplanes,
                   int disp_w, int disp_h, int buf_w, int buf_h);

/* Set palette for a layer (12-bit 0x0RGB), AGA-banked up to 256 entries. Layer 0
 * -> colours 0.., layer 1 (PF2) -> colours 2^pfplanes.. (the engine sets the
 * BPLCON3 PF2 colour offset to match: 8 for 3+3, 16 for 4+4). */
void hwscroll_palette(hwscroll_t *s, int layer, const uint16_t *rgb12, int n);
/* AGA 8-bit-per-channel (24-bit) palette via LOCT. rgb = 3 bytes/colour (R,G,B
 * 0-255). Use for palettes that 12-bit quantises into wrong hues. */
void hwscroll_palette8(hwscroll_t *s, int layer, const uint8_t *rgb, int n);

/* Plot a pen into a layer's buffer (helper for drawing the world/tiles). */
void hwscroll_putpix(hwscroll_t *s, int layer, int x, int y, uint8_t pen);
uint8_t *hwscroll_buffer(hwscroll_t *s, int layer, int plane);  /* raw plane ptr  */

/* Scroll a layer so buffer pixel (x,y) is at the display top-left (x fine+coarse,
 * y line-coarse). Layers scroll independently => parallax. */
void hwscroll_set(hwscroll_t *s, int layer, int x, int y);

/* Scroll a layer with a HUD split: scroll by `scroll` only between display lines
 * [y_top, y_bot); static (0) above y_top and at/below y_bot. Copper repositions
 * this layer's bitplane ptrs + PF1 fine nibble at the two boundaries while the
 * other playfield keeps scrolling. y_top/y_bot are DISPLAY lines (buffer rows).
 * Dual-pf layer 0 (PF1) only. Games not calling this are unaffected. */
void hwscroll_set_split(hwscroll_t *s, int layer, int scroll, int y_top, int y_bot);

/* Re-latch one layer's coarse pointers + fine scroll at a copper WAIT just before the
 * first display line (the same WAIT-gated latch the FG play band uses), curing the
 * continuous shimmer a hardware-scrolled dual-pf BACK playfield shows when its fine
 * scroll is applied only via the frame-top (no-WAIT) BPLCON1 slot. Call per frame after
 * hwscroll_set()/_set_split(). Games that never call it are unaffected. */
void hwscroll_layer_repos(hwscroll_t *s, int layer, int scroll);

/* Single-playfield HUD band split (nlayers==1). Display lines [0,play_top) and
 * [play_bot,disp_h) read from hud_bpl (a dedicated N-plane buffer, stride == s->stride,
 * planes spaced hud_plane_sz apart); the play band [play_top,play_bot) reads from
 * bg_bpl scrolled so display line play_top shows bg buffer row (bg_v+play_top). The
 * frame-top bitplane pointers are overridden to the HUD top band, so call this AFTER
 * hwscroll_set(), once per frame. Lines are DISPLAY lines (internal vcrop applied).
 * Inert if never called -> other games unaffected. */
void hwscroll_hud_bands(hwscroll_t *s, uint8_t *bg_bpl, uint8_t *hud_bpl,
                        long hud_plane_sz, int bg_v, int play_top, int play_bot);

/* Hardware sprites (8, each 16px wide x HWS_SPR_H tall, composited over the
 * playfields by the chipset -- they do NOT scroll with the background). img is
 * HWS_SPR_H lines x 2 words (word0 = plane-0 bits MSB-left, word1 = plane-1 bits)
 * giving pens 0-3 (0 = transparent). (x,y) are screen pixels; visible=0 hides it.
 * Sprite colours live in COLOR17-19 (sprites 0/1), 21-23 (2/3), etc. -- set via
 * hwscroll_sprite_colour(). */
void hwscroll_sprite(hwscroll_t *s, int idx, int x, int y, const uint16_t *img, int visible);
void hwscroll_sprite_colour(hwscroll_t *s, int idx, int pen, uint16_t rgb12);  /* pen 1-3 */

/* Sprite MULTIPLEXING: reuse the 8 DMA channels several times down the screen to
 * show many more than 8 sprites. Call _clear(), then _add() per sprite (feed them
 * sorted by ascending y for best packing), then _finish(). Each channel chains
 * vertically-non-overlapping sprites; _add() greedily picks a free channel and
 * returns 1 if placed, 0 if all channels are busy at that y (sprite dropped).
 * All multiplexed sprites share the channel colour groups set via
 * hwscroll_sprite_colour(). Up to HWS_SPR_SLOTS sprites per channel. */
#define HWS_SPR_SLOTS 12
void hwscroll_sprites_clear(hwscroll_t *s);
int  hwscroll_sprites_add(hwscroll_t *s, int x, int y, const uint16_t *img);
void hwscroll_sprites_finish(hwscroll_t *s);

/* ---- ATTACHED-PAIR hardware sprites (LOSSLESS 15-colour) ----
 * Each on-screen sprite is composited by TWO hw channels attached into a 4-bitplane
 * (16-colour, pen 0 transparent) sprite. 8 channels => 4 attached PAIRS live at once;
 * each pair MULTIPLEXES vertically (reuse a pair lower on the screen). All attached
 * pairs read COLOR16..31, so the per-band 15-colour palette is reloaded by the COPPER
 * just above each sprite -- meaning at most ONE distinct sprite palette can be live on
 * any single scanline. The router (in hwscroll_aspr_add) enforces that: a sprite is
 * placed in hardware only if no already-placed sprite of a DIFFERENT palette overlaps
 * its vertical band; otherwise it returns 0 and the caller falls back to a blitter bob.
 *
 * Palette identity is the caller's `palid` (Terra uses the sprite color_full): same
 * palid == identical 15 colours, so same-object multi-tile sprites coexist freely.
 *
 *   img4  = HWS_SPR_H lines x 4 words  {plane0,plane1,plane2,plane3} (pen 0..15 MSB-left)
 *   pal15 = 15 x uint16 (12-bit 0x0RGB) loaded into COLOR17..COLOR31 (pen 1..15)
 *
 * Feed sorted by ASCENDING screen-y. Returns 1 if placed in hardware, 0 -> bob. */
#define HWS_ASPR_PAIRS  4
#define HWS_ASPR_SLOTS  12        /* multiplex depth per pair (sprites per pair/frame) */
void hwscroll_aspr_clear(hwscroll_t *s);
int  hwscroll_aspr_add(hwscroll_t *s, int x, int y, const uint16_t *img4,
                       int palid, const uint16_t *pal15);
void hwscroll_aspr_finish(hwscroll_t *s);
int  hwscroll_aspr_count(hwscroll_t *s);   /* sprites placed in hardware this frame */

/* OBJECT-AWARE placement helper: snapshot the multiplex cursors so a caller can try
 * placing every tile of a multi-tile OBJECT (metasprite) and, if any tile won't fit
 * the hw-sprite budget, ROLL BACK and render the whole object in software -- so a
 * large object is never fragmented across multiplex bands (all-hw or all-software).
 * save() captures the current engine state; restore() rewinds to it (the sprite-data
 * words written past the rewound cursors are harmless: _finish terminates each chain
 * at the final cursor, so they sit beyond the list end). */
typedef struct { int cur[HWS_ASPR_PAIRS]; int lasty[HWS_ASPR_PAIRS]; int n; }
        hwscroll_aspr_state_t;
void hwscroll_aspr_save(hwscroll_t *s, hwscroll_aspr_state_t *st);
void hwscroll_aspr_restore(hwscroll_t *s, const hwscroll_aspr_state_t *st);

/* Wait for vblank, re-assert copper/DMA. Once per frame. */
void hwscroll_frame(hwscroll_t *s);
void hwscroll_frame_nowait(hwscroll_t *s);

#endif
