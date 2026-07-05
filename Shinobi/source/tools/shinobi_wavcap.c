/* shinobi_wavcap.c -- HOST audio-quality recorder. Drives the REAL Shinobi main
 * CPU + REAL sound engine (hal/shinobi_sound.c + shinobi_ym2151.cpp + ymfm) and
 * records the exact shinobi_sound_render() output the Amiga would play, at a
 * correct 22050 Hz rate (samples/video-frame = 22050/60 so game time and audio
 * time stay locked, matching real hardware's ~83k Z80 cycles/frame). Writes a
 * 16-bit mono WAV and prints objective artifact metrics, including a count of
 * loud near-Nyquist "buzz" windows (the YM2151-percussion aliasing that was the
 * reported "crash noise").
 *
 * env: SHINOBI_ROMS dir, SHINOBI_OUT prefix (default /tmp/shinobi_host),
 *      SHINOBI_COIN 0/1 (default 0), SHINOBI_WARM frames (default 60),
 *      SHINOBI_SECS seconds (default 45).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SH_SR 22050

extern int  shinobi_dyntrans_init(void);
extern void shinobi_dyntrans_frame(void);
extern void shinobi_dyntrans_set_inputs(uint8_t p1, uint8_t p2, uint8_t svc, uint8_t d1, uint8_t d2);
extern void shinobi_hw_open(void);
extern void shinobi_sound_init(void);
extern void shinobi_sound_render(signed char *out, int n);
extern int         shinobi_assets_load(void);
extern const char *shinobi_assets_error(void);
extern void        shinobi_assets_host_set_dir(const char *dir);

static int inject_coin;
static int warm_frames;

static void set_frame_inputs(int fr)
{
    uint8_t svc = 0xff, p1 = 0xff;
    if (inject_coin) {
        if (fr >= 60 && fr < 68) svc &= ~0x01;
        if (fr >= 120 && fr < 128) { svc &= ~0x10; p1 &= ~0x80; }
        if (fr >= 160 && (fr % 11) < 4) p1 &= ~0x5f;
    }
    shinobi_dyntrans_set_inputs(p1, 0xff, svc, 0xff, 0xff);
}

static void write_wav16(const char *path, const short *s, long n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    uint32_t datalen = (uint32_t)(n * 2), riff = 36 + datalen;
    uint16_t ch = 1, bits = 16, fmt = 1, align = 2; uint32_t rate = SH_SR, byterate = SH_SR * 2, sixteen = 16;
    fwrite("RIFF",1,4,f); fwrite(&riff,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); fwrite(&sixteen,4,1,f); fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f);
    fwrite(&rate,4,1,f); fwrite(&byterate,4,1,f); fwrite(&align,2,1,f); fwrite(&bits,2,1,f);
    fwrite("data",1,4,f); fwrite(&datalen,4,1,f); fwrite(s,2,(size_t)n,f); fclose(f);
}

int main(void)
{
    const char *romdir = getenv("SHINOBI_ROMS");
    if (romdir && romdir[0]) shinobi_assets_host_set_dir(romdir);
    const char *outp = getenv("SHINOBI_OUT"); if (!outp || !outp[0]) outp = "/tmp/shinobi_host";
    inject_coin = (getenv("SHINOBI_COIN") && getenv("SHINOBI_COIN")[0] == '1');
    warm_frames = getenv("SHINOBI_WARM") ? atoi(getenv("SHINOBI_WARM")) : 60;
    int secs = getenv("SHINOBI_SECS") ? atoi(getenv("SHINOBI_SECS")) : 45;
    if (secs <= 0) secs = 45;
    long total = (long)secs * SH_SR;

    signed char *post = malloc(total);
    short *wav = malloc(total * sizeof(short));
    if (!post || !wav) { fprintf(stderr, "oom\n"); return 1; }

    if (!shinobi_dyntrans_init()) { fprintf(stderr, "init fail: %s\n", shinobi_assets_error()); return 1; }
    shinobi_hw_open();
    shinobi_sound_init();
    shinobi_dyntrans_set_inputs(0xff, 0xff, 0xff, 0xff, 0xff);
    for (int w = 0; w < warm_frames; w++) { set_frame_inputs(w); shinobi_dyntrans_frame(); }

    long got = 0, prev = 0, frame_idx = 0; int fr = warm_frames;
    signed char tmp[1024];
    while (got < total) {
        set_frame_inputs(fr++);
        shinobi_dyntrans_frame();
        long target = ((frame_idx + 1) * (long)SH_SR) / 60; int n = (int)(target - prev);
        prev = target; frame_idx++;
        while (n > 0 && got < total) {
            int c = n > 1024 ? 1024 : n; if (got + c > total) c = (int)(total - got);
            shinobi_sound_render(tmp, c);
            memcpy(post + got, tmp, c); got += c; n -= c;
        }
    }

    for (long i = 0; i < got; i++) wav[i] = (short)(post[i] * 256);
    char path[512]; snprintf(path, sizeof path, "%s_mix.wav", outp);
    write_wav16(path, wav, got);

    /* metrics: rms, rails, and loud near-Nyquist "buzz" windows (sign flips on
     * >70% of a 64-sample window while the window is loud = aliased crash noise) */
    long rails = 0, nz = 0; double sq = 0;
    for (long i = 0; i < got; i++) { int v = post[i]; if (v >= 127 || v <= -128) rails++; if (v) nz++; sq += (double)v*v; }
    double rms = sqrt(sq / (got ? got : 1));
    const int W = 64; long buzz = 0, wins = 0; double gpk2 = 1;
    for (long i = 0; i < got; i++) { double e = (double)post[i]*post[i]; if (e > gpk2) gpk2 = e; }
    for (long i = 0; i + W <= got; i += W) {
        int sc = 0; double e = 0;
        for (int j = 1; j < W; j++) { if ((post[i+j-1] < 0) != (post[i+j] < 0)) sc++; e += (double)post[i+j]*post[i+j]; }
        wins++;
        if (sc > (int)(0.7 * W) && (e / W) > 0.05 * gpk2) buzz++;
    }
    printf("wavcap coin=%d secs=%d: n=%ld nz=%.1f%% rms=%.2f rails=%ld buzzWindows=%ld/%ld\n",
           inject_coin, secs, got, 100.0*nz/got, rms, rails, buzz, wins);
    fprintf(stderr, "wrote %s\n", path);
    free(post); free(wav);
    return 0;
}
