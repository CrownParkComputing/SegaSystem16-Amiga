/* shinobi_rtg_main.c -- Shinobi (System 16B) RTG/chunky 864x486 presenter.
 *
 * Opens the same class of 8-bit Picasso96 screen used by the working RTG ports,
 * renders the native 320x224 arcade frame to a chunky buffer, scales it to the
 * presenter, and WriteChunkyPixels it.
 * Built with -DSHINOBI_RTG (shinobi_hwrender.c stops at the chunky pen frame).
 */
#include <exec/exec.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <stdint.h>
#include <string.h>
#include "arcade_intro.h"
#include "shinobi_rtg_bezel.h"

extern int  shinobi_dyntrans_init(void);
extern void shinobi_dyntrans_frame(void);
extern int  shinobi_dyntrans_rendered(void);
extern void shinobi_dyntrans_set_inputs(uint8_t,uint8_t,uint8_t,uint8_t,uint8_t);
extern void shinobi_hw_open(void);
extern void shinobi_hw_close(void);
extern void shinobi_audio_amiga_open(void);
extern void shinobi_audio_amiga_frame(void);
extern void shinobi_audio_amiga_close(void);
extern const unsigned char *shinobi_chunky(void);     /* SW*SH chunky pens */
extern const unsigned char *shinobi_pal256(int *n);   /* reduced 256-pen RGB */
extern void shinobi_dims(int *w, int *h);
extern const unsigned char ai_default_mod[], ai_default_mod_end[];
extern const unsigned char shinobi_rtg_bezel[], shinobi_rtg_bezel_end[];

struct IntuitionBase *IntuitionBase = 0;
extern struct GfxBase *GfxBase;                 /* defined in shinobi_hwrender.c (shared) */
struct Device *TimerBase = 0;

#define RTG_W       864
#define RTG_H       486
#define RTG_MODE_ID 0x50FF1000UL                /* uaegfx/Picasso96 8-bit mode used by the working RTG ports */

static struct Screen *scr;
static struct Window *win;
static struct MsgPort *timer_port;
static struct timerequest *timer_io;
static uint8_t *rtg_frame;
static uint32_t loadrgb[1 + 256*3 + 1];
static uint8_t last_pal[256*3];
static int g_ok, SW_, SH_, rtg_ok, rtg_w, rtg_h;
static int last_pal_n = -1;
static int dispX, dispY, dispW, dispH, layout_ready;
static int bezel_active;
#define DISP_MAXW 1920
#define DISP_MAXH 1080
static int colmap[DISP_MAXW], rowmap[DISP_MAXH];
static uint8_t keydown[128];
static uint8_t shinobi_dsw1 = 0xff, shinobi_dsw2 = 0x7c;

static volatile int g_quit;
static ULONG eclock_rate, frame_ticks, next_tick;

#define RK_1      0x01
#define RK_5      0x05
#define RK_SPACE  0x40
#define RK_ESC    0x45
#define RK_F10    0x59
#define RK_LCTRL  0x63
#define RK_LALT   0x64
#define RK_RALT   0x65
#define RK_UP     0x4C
#define RK_DOWN   0x4D
#define RK_RIGHT  0x4E
#define RK_LEFT   0x4F
#define CIAA_PRA  (*(volatile unsigned char *)0xbfe001UL)
#define JOY1DAT   (*(volatile unsigned short *)0xdff00cUL)
#define POTGO     (*(volatile unsigned short *)0xdff034UL)
#define POTINP    (*(volatile unsigned short *)0xdff016UL)
#define PORT1_FIRE 0x80
#define CD32_DATRY 0x4000

static const char *const shinobi_intro_keys[] = {
    "ARROWS MOVE", "SPACE / CTRL ATTACK", "ALT SHURIKEN",
    "5 COIN   1 START", "F10 DIP SWITCHES", "ESC EXIT", 0
};
static const char *const shinobi_intro_pad[] = {
    "STICK MOVE", "RED ATTACK", "BLUE / YELLOW SHURIKEN",
    "L / R COIN   PLAY START", "L + R + PLAY DIP SWITCHES", 0
};
static const ai_dip_opt shinobi_coin_a[] = {
    {0x07,"4C 1C"}, {0x08,"3C 1C"}, {0x09,"2C 1C"}, {0x05,"2C 1C 5/3 6/4"},
    {0x04,"2C 1C 4/3"}, {0x0f,"1C 1C"}, {0x03,"1C 1C 5/6"},
    {0x02,"1C 1C 4/5"}, {0x01,"1C 1C 2/3"}, {0x06,"2C 3C"},
    {0x0e,"1C 2C"}, {0x0d,"1C 3C"}, {0x0c,"1C 4C"}, {0x0b,"1C 5C"},
    {0x0a,"1C 6C"}, {0x00,"FREE/1C 1C"}
};
static const ai_dip_opt shinobi_coin_b[] = {
    {0x70,"4C 1C"}, {0x80,"3C 1C"}, {0x90,"2C 1C"}, {0x50,"2C 1C 5/3 6/4"},
    {0x40,"2C 1C 4/3"}, {0xf0,"1C 1C"}, {0x30,"1C 1C 5/6"},
    {0x20,"1C 1C 4/5"}, {0x10,"1C 1C 2/3"}, {0x60,"2C 3C"},
    {0xe0,"1C 2C"}, {0xd0,"1C 3C"}, {0xc0,"1C 4C"}, {0xb0,"1C 5C"},
    {0xa0,"1C 6C"}, {0x00,"FREE/1C 1C"}
};
static const ai_dip_opt shinobi_cabinet[] = { {0x00,"UPRIGHT"}, {0x01,"COCKTAIL"} };
static const ai_dip_opt shinobi_demo[] = { {0x02,"OFF"}, {0x00,"ON"} };
static const ai_dip_opt shinobi_lives[] = { {0x08,"2"}, {0x0c,"3"}, {0x04,"5"}, {0x00,"FREE PLAY"} };
static const ai_dip_opt shinobi_diff[] = { {0x20,"EASY"}, {0x30,"NORMAL"}, {0x10,"HARD"}, {0x00,"HARDEST"} };
static const ai_dip_opt shinobi_bullet[] = { {0x40,"SLOW"}, {0x00,"FAST"} };
static const ai_dip_opt shinobi_lang[] = { {0x80,"JAPANESE"}, {0x00,"ENGLISH"} };
static const ai_dip_item shinobi_dip_items[] = {
    {"COIN A",0,0x0f,16,shinobi_coin_a},
    {"COIN B",0,0xf0,16,shinobi_coin_b},
    {"CABINET",1,0x01,2,shinobi_cabinet},
    {"DEMO SOUNDS",1,0x02,2,shinobi_demo},
    {"LIVES",1,0x0c,4,shinobi_lives},
    {"DIFFICULTY",1,0x30,4,shinobi_diff},
    {"BULLET SPEED",1,0x40,2,shinobi_bullet},
    {"LANGUAGE",1,0x80,2,shinobi_lang}
};
static void shinobi_apply_dips(void *ctx)
{
    (void)ctx;
    shinobi_dyntrans_set_inputs(0xff, 0xff, 0xff, shinobi_dsw1, shinobi_dsw2);
}
static const ai_dip_config shinobi_dip_cfg = {
    shinobi_dip_items, (int)(sizeof shinobi_dip_items / sizeof shinobi_dip_items[0]),
    &shinobi_dsw1, &shinobi_dsw2,
    shinobi_apply_dips, 0
};
static int shinobi_intro_ready(void *ctx) { (void)ctx; return g_ok; }
static void shinobi_intro_warmup(void *ctx)
{
    (void)ctx;
    if (!g_ok) {
        g_ok = shinobi_dyntrans_init();
        if (g_ok)
            shinobi_dyntrans_set_inputs(0xff, 0xff, 0xff, shinobi_dsw1, shinobi_dsw2);
    }
}
static const ai_config shinobi_intro_cfg = {
    "SHINOBI",
    "WHITTY ARCADE PRESENTS SHINOBI    SEGA 1987 SYSTEM 16B HARDWARE    MAIN 68000 RUNS IN A MUSASHI INTERPRETER WITH ORIGINAL TILEMAP TEXT SPRITE AND SCROLL RAM PRESENTED THROUGH AN 864 BY 486 RTG SCREEN    ROMS ARE LOADED AT RUNTIME FROM YOUR SHARED ROM FOLDER    PRESS FIRE OR START WHEN READY    ",
    shinobi_intro_keys, shinobi_intro_pad,
    ai_default_mod, ai_default_mod_end,
    0, 150,
    shinobi_intro_ready, shinobi_intro_warmup, 0,
    &shinobi_dip_cfg
};

int hal_game_should_exit(void)
{
    return g_quit;
}

static struct RastPort *present_rp(void)
{
    return win ? win->RPort : &scr->RastPort;
}

static void close_timer(void)
{
    if (timer_io) {
        if (TimerBase)
            CloseDevice((struct IORequest*)timer_io);
        DeleteIORequest((struct IORequest*)timer_io);
        timer_io = 0;
    }
    if (timer_port) {
        DeleteMsgPort(timer_port);
        timer_port = 0;
    }
    TimerBase = 0;
    eclock_rate = frame_ticks = next_tick = 0;
}

static void open_timer(void)
{
    struct EClockVal ev;
    timer_port = CreateMsgPort();
    if (!timer_port)
        return;
    timer_io = (struct timerequest*)CreateIORequest(timer_port, sizeof(*timer_io));
    if (!timer_io) {
        close_timer();
        return;
    }
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest*)timer_io, 0) != 0) {
        close_timer();
        return;
    }
    TimerBase = timer_io->tr_node.io_Device;
    eclock_rate = ReadEClock(&ev);
    frame_ticks = (eclock_rate + 30) / 60;
    if (frame_ticks < 1)
        frame_ticks = 1;
    next_tick = ev.ev_lo;
}

static void frame_pace(void)
{
    struct EClockVal ev;
    ULONG now;
    if (!TimerBase || !frame_ticks)
        return;
    ReadEClock(&ev);
    now = ev.ev_lo;
    if ((LONG)(now - next_tick) > (LONG)frame_ticks) {
        next_tick = now;
        return;
    }
    next_tick += frame_ticks;
    do {
        ReadEClock(&ev);
        now = ev.ev_lo;
    } while ((LONG)(now - next_tick) < 0);
}

static void invalidate_palette_cache(void)
{
    last_pal_n = -1;
}

/* DIAG: flood the RTG screen with one solid colour (pen 1 = r,g,b) so we can see,
 * via a screenshot, exactly which stage was last reached before a hang. */
static void diag_fill(uint8_t r, uint8_t g, uint8_t b)
{
    if (!rtg_ok || !scr || !rtg_frame) return;
    uint32_t lr[1 + 3 + 1];
    lr[0] = (1u << 16) | 1u;           /* load 1 colour starting at pen 1 */
    lr[1] = (uint32_t)r * 0x01010101u;
    lr[2] = (uint32_t)g * 0x01010101u;
    lr[3] = (uint32_t)b * 0x01010101u;
    lr[4] = 0;
    invalidate_palette_cache();
    LoadRGB32(&scr->ViewPort, lr);
    memset(rtg_frame, 1, RTG_W * RTG_H);
    SetAPen(present_rp(), 1);
    RectFill(present_rp(), 0, 0, RTG_W-1, RTG_H-1);
}

static void compute_layout(void)
{
    int i;
    int bx = 0, by = 0, bw = rtg_w, bh = rtg_h;
    if (rtg_w == SHINOBI_RTG_W && rtg_h == SHINOBI_RTG_H) {
        bx = SHINOBI_GAME_X;
        by = SHINOBI_GAME_Y;
        bw = SHINOBI_GAME_W;
        bh = SHINOBI_GAME_H;
    }
    dispW = bw;
    dispH = (bw * SH_) / SW_;
    if (dispH > bh) {
        dispH = bh;
        dispW = (bh * SW_) / SH_;
    }
    if (dispW > rtg_w) dispW = rtg_w;
    if (dispH > rtg_h) dispH = rtg_h;
    if (dispW > SW_ * 2 || dispH > SH_ * 2) {
        dispW = SW_ * 2;
        dispH = SH_ * 2;
        if (dispW > bw) {
            dispW = bw;
            dispH = (bw * SH_) / SW_;
        }
        if (dispH > bh) {
            dispH = bh;
            dispW = (bh * SW_) / SH_;
        }
    }
    if (dispW > DISP_MAXW) dispW = DISP_MAXW;
    if (dispH > DISP_MAXH) dispH = DISP_MAXH;
    dispX = bx + (bw - dispW) / 2;
    dispY = by + (bh - dispH) / 2;
    if (dispX < 0) dispX = 0;
    if (dispY < 0) dispY = 0;
    for (i = 0; i < dispW; i++) colmap[i] = i * SW_ / dispW;
    for (i = 0; i < dispH; i++) rowmap[i] = i * SH_ / dispH;
    layout_ready = 1;
}

static void restore_rtg_bezel(void)
{
    size_t n;
    if (!rtg_ok || !scr || !rtg_frame)
        return;
    n = (size_t)rtg_w * rtg_h;
    if (rtg_w == SHINOBI_RTG_W && rtg_h == SHINOBI_RTG_H &&
        (size_t)(shinobi_rtg_bezel_end - shinobi_rtg_bezel) >= n) {
        memcpy(rtg_frame, shinobi_rtg_bezel, n);
    } else {
        memset(rtg_frame, 0, n);
    }
    WriteChunkyPixels(present_rp(), 0, 0, rtg_w - 1, rtg_h - 1, rtg_frame, rtg_w);
    bezel_active = 1;
}

/* nearest-neighbour stretch of the SW_xSH_ chunky frame into a centred play rect */
static void scale_to_rtg(void)
{
    const unsigned char *src = shinobi_chunky();
    int last_sy = -1;
    if (!layout_ready) compute_layout();
    if (dispW == SW_ * 2 && dispH == SH_ * 2) {
        for (int sy = 0; sy < SH_; sy++) {
            const uint8_t *srow = src + sy * SW_;
            uint8_t *dst0 = rtg_frame + (size_t)(dispY + sy * 2) * rtg_w + dispX;
            uint8_t *dst1 = dst0 + rtg_w;
            for (int x = 0; x < SW_; x++) {
                uint8_t p = srow[x];
                dst0[x * 2] = p;
                dst0[x * 2 + 1] = p;
            }
            memcpy(dst1, dst0, dispW);
        }
        return;
    }
    for (int y = 0; y < dispH; y++) {
        int sy = rowmap[y];
        uint8_t *dst = rtg_frame + (size_t)(dispY + y) * rtg_w + dispX;
        if (sy == last_sy) {
            memcpy(dst, dst - rtg_w, dispW);
            continue;
        }
        last_sy = sy;
        const unsigned char *srow = src + sy*SW_;
        for (int x = 0; x < dispW; x++) dst[x] = srow[colmap[x]];
    }
}

static void upload_palette(void)
{
    int n; const unsigned char *pal = shinobi_pal256(&n);
    if (!scr || !pal || n <= 0) return;
    if (n == last_pal_n && memcmp(last_pal, pal, (size_t)n * 3) == 0)
        return;
    memcpy(last_pal, pal, (size_t)n * 3);
    last_pal_n = n;
    loadrgb[0] = ((uint32_t)n << 16) | 0;
    for (int i = 0; i < n; i++) {
        loadrgb[1+i*3+0] = (uint32_t)pal[i*3+0] * 0x01010101u;
        loadrgb[1+i*3+1] = (uint32_t)pal[i*3+1] * 0x01010101u;
        loadrgb[1+i*3+2] = (uint32_t)pal[i*3+2] * 0x01010101u;
    }
    loadrgb[1+n*3] = 0;
    LoadRGB32(&scr->ViewPort, loadrgb);
}

static void clear_screen_black(void)
{
    if (!rtg_ok || !scr || !rtg_frame) return;
    uint32_t lr[1 + 3 + 1];
    lr[0] = (1u << 16) | 0u;
    lr[1] = 0;
    lr[2] = 0;
    lr[3] = 0;
    lr[4] = 0;
    invalidate_palette_cache();
    LoadRGB32(&scr->ViewPort, lr);
    memset(rtg_frame, 0, (size_t)rtg_w * rtg_h);
    WriteChunkyPixels(present_rp(), 0, 0, rtg_w - 1, rtg_h - 1, rtg_frame, rtg_w);
    bezel_active = 0;
}

static void shutdown_rtg(void)
{
    static int done;
    if (done) return;
    done = 1;
    rtg_ok = 0;
    layout_ready = 0;
    invalidate_palette_cache();
    shinobi_audio_amiga_close();
    close_timer();
    if (GfxBase) WaitTOF();
    if (win) { CloseWindow(win); win = 0; }
    if (rtg_frame) {
        FreeMem(rtg_frame, (size_t)rtg_w * rtg_h);
        rtg_frame = 0;
    }
    if (scr) { CloseScreen(scr); scr = 0; }
    shinobi_hw_close();
    if (GfxBase) { CloseLibrary((struct Library *)GfxBase); GfxBase = 0; }
    if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = 0; }
}

void hal_cleanup(void)
{
    shutdown_rtg();
}

static void poll_input(void)
{
    struct IntuiMessage *m;
    if (win && win->UserPort) {
        while ((m = (struct IntuiMessage*)GetMsg(win->UserPort))) {
            ULONG cls = m->Class;
            UWORD raw = m->Code;
            ReplyMsg((struct Message*)m);
            if (cls == IDCMP_RAWKEY)
                keydown[raw & 0x7f] = (raw & 0x80) ? 0 : 1;
        }
    }

    unsigned cd32 = ai_read_cd32_port1();
    static int kf10, pdip;
    int dip_now = ai_cd32_dip_combo(cd32);
    if ((keydown[RK_F10] && !kf10) || (dip_now && !pdip)) {
        shinobi_audio_amiga_close();
        ai_dip_open(&shinobi_dip_cfg);
        upload_palette();
        restore_rtg_bezel();
        shinobi_audio_amiga_open();
        keydown[RK_F10] = 0;
    }
    kf10 = keydown[RK_F10];
    pdip = dip_now;

    if (keydown[RK_ESC] || ai_cd32_exit_combo(cd32)) {
        g_quit = 1;
        return;
    }

    unsigned v = JOY1DAT;
    int right = (v >> 1) & 1;
    int left  = (v >> 9) & 1;
    int down  = ((v >> 1) ^ v) & 1;
    int up    = ((v >> 9) ^ (v >> 8)) & 1;
    if (keydown[RK_RIGHT]) right = 1;
    if (keydown[RK_LEFT])  left = 1;
    if (keydown[RK_DOWN])  down = 1;
    if (keydown[RK_UP])    up = 1;

    int fire1 = !(CIAA_PRA & PORT1_FIRE);
    int fire2 = !(POTINP & CD32_DATRY);
    int attack = fire1 || keydown[RK_SPACE] || keydown[RK_LCTRL] || (cd32 & AI_CD32_RED);
    int jump   = fire2 || keydown[RK_LALT] || keydown[RK_RALT] || (cd32 & (AI_CD32_BLUE | AI_CD32_YELLOW));
    int magic  = (cd32 & AI_CD32_GREEN) != 0;

    uint8_t svc = 0xff, p1 = 0xff;
    static int coin_prev, start_prev, coin_hold, start_hold;
    int coin = keydown[RK_5] || (cd32 & (AI_CD32_LSHOULDER | AI_CD32_RSHOULDER));
    int start = keydown[RK_1] || (cd32 & AI_CD32_PLAY);
    if (coin && !coin_prev) coin_hold = 10;
    if (start && !start_prev) start_hold = 24;
    coin_prev = coin;
    start_prev = start;

    if (coin_hold)  { svc &= (uint8_t)~0x01; coin_hold--; }
    if (start_hold) { svc &= (uint8_t)~0x10; start_hold--; }
    if (down)   p1 &= (uint8_t)~0x10;
    if (up)     p1 &= (uint8_t)~0x20;
    if (right)  p1 &= (uint8_t)~0x40;
    if (left)   p1 &= (uint8_t)~0x80;
    if (attack) p1 &= (uint8_t)~0x02;
    if (jump)   p1 &= (uint8_t)~0x04;
    if (magic)  p1 &= (uint8_t)~0x01;

    shinobi_dyntrans_set_inputs(p1, 0xff, svc, shinobi_dsw1, shinobi_dsw2);
}

void hal_game_init(void)
{
    shinobi_hw_open();
    shinobi_dims(&SW_, &SH_);

    IntuitionBase = (struct IntuitionBase*)OpenLibrary((CONST_STRPTR)"intuition.library",39);
    GfxBase       = (struct GfxBase*)OpenLibrary((CONST_STRPTR)"graphics.library",40);
    if (!IntuitionBase || !GfxBase) return;

    ULONG mode = BestModeID(BIDTAG_NominalWidth, RTG_W, BIDTAG_NominalHeight, RTG_H,
                            BIDTAG_DesiredWidth, RTG_W, BIDTAG_DesiredHeight, RTG_H,
                            BIDTAG_Depth, 8, TAG_DONE);
    if (mode == INVALID_ID) mode = RTG_MODE_ID;
    scr = OpenScreenTags(0, SA_DisplayID,mode, SA_Width,RTG_W, SA_Height,RTG_H,
                         SA_Depth,8, SA_Quiet,1, SA_Type,CUSTOMSCREEN, SA_ShowTitle,0, TAG_END);
    if (!scr)
        scr = OpenScreenTags(0, SA_Width,RTG_W, SA_Height,RTG_H, SA_Depth,8,
                             SA_Quiet,1, SA_Type,CUSTOMSCREEN, SA_ShowTitle,0, TAG_END);
    if (!scr) return;
    rtg_w = scr->Width;
    rtg_h = scr->Height;
    if (rtg_w < 1) rtg_w = RTG_W;
    if (rtg_h < 1) rtg_h = RTG_H;
    rtg_frame = (uint8_t*)AllocMem((size_t)rtg_w * rtg_h, MEMF_FAST|MEMF_CLEAR);
    if (!rtg_frame) rtg_frame = (uint8_t*)AllocMem((size_t)rtg_w * rtg_h, MEMF_PUBLIC|MEMF_CLEAR);
    if (!rtg_frame) return;
    win = OpenWindowTags(0, WA_CustomScreen,(ULONG)scr, WA_Left,0, WA_Top,0,
                         WA_Width,rtg_w, WA_Height,rtg_h, WA_Backdrop,TRUE, WA_Borderless,TRUE,
                         WA_Activate,TRUE, WA_RMBTrap,TRUE, WA_IDCMP,IDCMP_RAWKEY, TAG_END);
    if (win) { ScreenToFront(scr); ActivateWindow(win); }
    if (!win) return;
    open_timer();
    rtg_ok = 1;
    clear_screen_black();

    ai_init(scr, win, rtg_frame, rtg_w, rtg_h);
    ai_set_loader_enabled(1);
    if (!ai_run(&shinobi_intro_cfg)) { g_quit = 1; return; }
    if (!g_ok) { diag_fill(0xC0,0x00,0x00); return; }
    upload_palette();
    restore_rtg_bezel();
    shinobi_audio_amiga_open();
}

void hal_game_frame(void)
{
    if (!g_ok) { diag_fill(0xC0,0x00,0x00); return; }   /* RED steady: dyntrans_init failed */
    poll_input();
    if (g_quit) return;
    /* The translator is now USER-MODE-safe (virtual SR, CacheClearU cache flush) -- so
     * the whole frame, CPU + OS graphics, runs in user mode exactly like tigerh_rtg. */
    shinobi_dyntrans_frame();           /* if it stays GREEN, the hang is in here */
    shinobi_audio_amiga_frame();
    if (!rtg_ok) return;
    if (!shinobi_dyntrans_rendered()) {
        frame_pace();
        return;
    }
    if (last_pal_n < 0)
        upload_palette();
    if (!bezel_active)
        restore_rtg_bezel();
    scale_to_rtg();
    WriteChunkyPixels(present_rp(), dispX, dispY, dispX + dispW - 1, dispY + dispH - 1,
                      rtg_frame + (size_t)dispY * rtg_w + dispX, rtg_w);
    frame_pace();
}
