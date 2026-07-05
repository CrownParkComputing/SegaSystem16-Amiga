/* Shinobi Paula playback.
 *
 * Continuous-ring model copied from the working 1943/Saint Dragon audio path:
 * Paula free-runs over one larger chip-RAM ring while each video frame writes
 * fresh audio a fixed lead ahead of the estimated play cursor. This avoids the
 * stale tiny-buffer replay that sounds like reverb when RTG rendering stalls.
 */
#include <exec/exec.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <stdint.h>

extern void shinobi_sound_init(void);
extern void shinobi_sound_render(signed char *out, int n);
extern struct Device *TimerBase;

#define CUSTOM    ((volatile uint16_t *)0xdff000)
#define R_DMACON  (0x096/2)
#define R_AUD0LCH (0x0a0/2)
#define R_AUD0LEN (0x0a4/2)
#define R_AUD0PER (0x0a6/2)
#define R_AUD0VOL (0x0a8/2)
#define R_AUD1LCH (0x0b0/2)
#define R_AUD1LEN (0x0b4/2)
#define R_AUD1PER (0x0b6/2)
#define R_AUD1VOL (0x0b8/2)
#define R_AUD2LEN (0x0c4/2)
#define R_AUD2VOL (0x0c8/2)
#define R_AUD3LEN (0x0d4/2)
#define R_AUD3VOL (0x0d8/2)

#define SH_SR      22050
#define SH_PER     (3546895 / SH_SR)
#define SH_SPF     (SH_SR / 60 + 64)
#define LEAD_FR    3
#define RING_FR    24
#define SH_LEAD    (LEAD_FR * SH_SPF)
#define SH_RING    (RING_FR * SH_SPF)

static signed char *ring;
static unsigned long p_play;
static unsigned long p_wrote;
static unsigned long audio_rate;
static unsigned long last_tick;
static unsigned long frac_ticks;

static void aud_setup(volatile uint16_t *c)
{
    uint32_t a = (uint32_t)ring;
    c[R_AUD0LCH] = (uint16_t)(a >> 16); c[R_AUD0LCH + 1] = (uint16_t)a;
    c[R_AUD1LCH] = (uint16_t)(a >> 16); c[R_AUD1LCH + 1] = (uint16_t)a;
    c[R_AUD0LEN] = SH_RING / 2; c[R_AUD1LEN] = SH_RING / 2;
    c[R_AUD0PER] = SH_PER;      c[R_AUD1PER] = SH_PER;
    c[R_AUD0VOL] = 56;          c[R_AUD1VOL] = 56;
}

static void ring_render(unsigned long n)
{
    while (n) {
        unsigned long idx = p_wrote % SH_RING;
        unsigned long chunk = SH_RING - idx;
        if (chunk > n)
            chunk = n;
        shinobi_sound_render(ring + idx, (int)chunk);
        CacheClearE(ring + idx, chunk, CACRF_ClearD);
        p_wrote += chunk;
        n -= chunk;
    }
}

void shinobi_audio_amiga_open(void)
{
    volatile uint16_t *c = CUSTOM;
    if (ring)
        return;

    shinobi_sound_init();
    ring = (signed char *)AllocMem(SH_RING, MEMF_CHIP | MEMF_CLEAR);
    if (!ring)
        return;

    p_play = 0;
    p_wrote = 0;
    audio_rate = 0;
    last_tick = 0;
    frac_ticks = 0;
    if (TimerBase) {
        struct EClockVal ev;
        audio_rate = ReadEClock(&ev);
        last_tick = ev.ev_lo;
    }
    ring_render(SH_LEAD);

    c[R_DMACON] = 0x000f;
    c[R_AUD0VOL] = 0; c[R_AUD1VOL] = 0; c[R_AUD2VOL] = 0; c[R_AUD3VOL] = 0;
    c[R_AUD2LEN] = 0; c[R_AUD3LEN] = 0;
    aud_setup(c);
    c[R_DMACON] = 0x8203;
}

void shinobi_audio_amiga_frame(void)
{
    if (!ring)
        return;

    if (audio_rate && TimerBase) {
        struct EClockVal ev;
        unsigned long now;
        unsigned long delta;
        unsigned long long clocks;
        ReadEClock(&ev);
        now = ev.ev_lo;
        delta = now - last_tick;
        last_tick = now;
        clocks = (unsigned long long)frac_ticks + (unsigned long long)delta * SH_SR;
        p_play += (unsigned long)(clocks / audio_rate);
        frac_ticks = (unsigned long)(clocks % audio_rate);
    } else {
        p_play += SH_SR / 60u;
    }
    unsigned long target = p_play + SH_LEAD;
    unsigned long cap = p_play + (SH_RING - SH_SPF);
    if (target > cap)
        target = cap;
    if ((long)(target - p_wrote) > 0)
        ring_render(target - p_wrote);
}

void shinobi_audio_amiga_close(void)
{
    volatile uint16_t *c = CUSTOM;
    c[R_DMACON] = 0x000f;
    c[R_AUD0VOL] = 0; c[R_AUD1VOL] = 0; c[R_AUD2VOL] = 0; c[R_AUD3VOL] = 0;
    for (volatile unsigned i = 0; i < 50000; i++) ;
    if (ring) {
        FreeMem(ring, SH_RING);
        ring = 0;
    }
    p_play = 0;
    p_wrote = 0;
    audio_rate = 0;
    last_tick = 0;
    frac_ticks = 0;
}
