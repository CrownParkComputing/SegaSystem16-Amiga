/* shinobi_hostsnd.c -- HOST (Linux native cc) headless harness that drives the
 * REAL Shinobi (Sega System 16B, shinobi6) sound pipeline under ASan+UBSan to
 * reproduce the "sound keeps crashing" bug reported on device (amiberry).
 *
 * It runs the port's OWN engine unchanged:
 *   - hal/shinobi_interp.c   : Musashi 68000 main CPU + System-16B memory map.
 *     When the game writes the sound latch (mapper reg 0x03) it calls the REAL
 *     shinobi_audio_command().
 *   - hal/shinobi_sound.c    : Z80 + YM2151 (ymfm) + uPD7759 sample engine.
 *     shinobi_sound_render() mixes SH_SPF samples/frame exactly like the device.
 *
 * Two torture passes:
 *   1. GAME pass: insert coin+start, run N frames of the real program so the
 *      real Z80 sound driver issues real bank switches / sample plays / YM writes.
 *   2. SWEEP pass: force every possible latch value 0x00..0xff into
 *      shinobi_audio_command() and render 60 frames each, so every code path the
 *      sound driver could take on a bad/rare command is exercised.
 *
 * A per-render SIGALRM watchdog catches an unbounded loop / hang in the sound
 * path and dumps the interrupted stack (glibc backtrace) so the offending
 * function is pinpointed. ASan/UBSan catch any OOB read/write or UB with an
 * exact stack of their own.
 *
 * usage: shinobi_hostsnd [frames=20000] [roms_dir]
 *   env SHINOBI_ROMS  overrides the ROM directory.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/time.h>
#include <execinfo.h>
#include <unistd.h>

#define SH_SPF 431          /* samples per frame at 22050Hz (~1/50s), device value */

/* engine entry points (hal/shinobi_interp.c) */
extern int  shinobi_dyntrans_init(void);
extern void shinobi_dyntrans_frame(void);
extern void shinobi_dyntrans_set_inputs(uint8_t p1, uint8_t p2, uint8_t svc,
                                        uint8_t d1, uint8_t d2);

/* renderer entry points (hal/shinobi_hwrender.c) */
extern void shinobi_hw_open(void);

/* real sound engine (hal/shinobi_sound.c) */
extern void    shinobi_sound_init(void);
extern void    shinobi_sound_render(signed char *out, int n);
extern void    shinobi_audio_command(uint8_t v);
extern uint8_t shinobi_audio_response(void);

/* debug counters exposed by shinobi_sound.c */
extern unsigned shinobi_sound_dbg_ym_writes(void);
extern unsigned shinobi_sound_dbg_keyons(void);
extern unsigned shinobi_sound_dbg_sample_writes(void);
extern unsigned shinobi_sound_dbg_commands(void);
extern unsigned shinobi_sound_dbg_high_commands(void);
extern unsigned shinobi_sound_dbg_last_command(void);
extern unsigned shinobi_sound_dbg_pcm_nonzero(void);

/* host assets (tools/shinobi_hostsnd_assets.c) */
extern int         shinobi_assets_load(void);
extern const char *shinobi_assets_error(void);
extern void        shinobi_assets_host_set_dir(const char *dir);

static volatile sig_atomic_t g_frame;
static volatile sig_atomic_t g_phase;   /* 0=game, 1=sweep */
static volatile sig_atomic_t g_cmd;

static void watchdog(int sig)
{
    (void)sig;
    char msg[160];
    int n = snprintf(msg, sizeof msg,
        "\n*** WATCHDOG: shinobi_sound_render hung >4s "
        "(phase=%s frame=%ld cmd=0x%02lx) -- UNBOUNDED LOOP ***\n",
        g_phase ? "sweep" : "game", (long)g_frame, (long)g_cmd);
    if (n > 0) { ssize_t w = write(2, msg, (size_t)n); (void)w; }
    void *bt[64];
    int m = backtrace(bt, 64);
    backtrace_symbols_fd(bt, m, 2);
    _exit(42);
}

/* run one guarded render into buf; watchdog fires if it hangs. */
static void guarded_render(signed char *buf, int n)
{
    struct itimerval it;
    it.it_value.tv_sec = 4; it.it_value.tv_usec = 0;
    it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &it, NULL);
    shinobi_sound_render(buf, n);
    it.it_value.tv_sec = 0;
    setitimer(ITIMER_REAL, &it, NULL);   /* disarm */
}

int main(int argc, char **argv)
{
    int frames = (argc > 1) ? atoi(argv[1]) : 20000;
    if (frames <= 0) frames = 20000;

    const char *romdir = (argc > 2) ? argv[2] : getenv("SHINOBI_ROMS");
    if (romdir && romdir[0]) shinobi_assets_host_set_dir(romdir);

    signal(SIGALRM, watchdog);

    if (!shinobi_dyntrans_init()) {
        fprintf(stderr, "shinobi_dyntrans_init failed: %s\n", shinobi_assets_error());
        return 1;
    }
    shinobi_hw_open();
    shinobi_sound_init();
    shinobi_dyntrans_set_inputs(0xff, 0xff, 0xff, 0xff, 0xff);

    /* heap buffer so any over-run past SH_SPF is flagged by ASan */
    signed char *buf = (signed char *)malloc(SH_SPF);
    if (!buf) { fprintf(stderr, "oom audio buffer\n"); return 1; }

    fprintf(stderr, "== GAME pass: %d frames, coin@300 start@360, real sound ==\n", frames);
    g_phase = 0;
    for (int fr = 1; fr <= frames; fr++) {
        g_frame = fr;
        /* System-16B service port active-low: bit0=coin1, bit4=start1. */
        uint8_t svc = 0xff, p1 = 0xff;
        if (fr >= 300 && fr < 308) svc &= ~0x01;                 /* coin1  */
        if (fr >= 360 && fr < 368) { svc &= ~0x10; p1 &= ~0x80; } /* start1 */
        /* After start, mash P1 action bits periodically so gameplay SFX fire
         * (shurikens / ninja magic drive both YM2151 and uPD7759 heavily). */
        if (fr >= 400 && (fr % 11) < 4) p1 &= ~0x5f;
        shinobi_dyntrans_set_inputs(p1, 0xff, svc, 0xff, 0xff);

        shinobi_dyntrans_frame();     /* real game -> real sound-latch writes */
        guarded_render(buf, SH_SPF);  /* real Z80+YM+uPD mix, ASan/UBSan armed */

        if (fr % 2000 == 0)
            fprintf(stderr,
                "  [fr %5d] cmds=%u last=0x%02x ymW=%u keyon=%u smpW=%u pcmNZ=%u resp=0x%02x\n",
                fr, shinobi_sound_dbg_commands(), shinobi_sound_dbg_last_command(),
                shinobi_sound_dbg_ym_writes(), shinobi_sound_dbg_keyons(),
                shinobi_sound_dbg_sample_writes(), shinobi_sound_dbg_pcm_nonzero(),
                shinobi_audio_response());
    }
    fprintf(stderr, "  GAME pass done: total cmds=%u high=%u ymW=%u keyon=%u smpW=%u\n",
            shinobi_sound_dbg_commands(), shinobi_sound_dbg_high_commands(),
            shinobi_sound_dbg_ym_writes(), shinobi_sound_dbg_keyons(),
            shinobi_sound_dbg_sample_writes());

    fprintf(stderr, "== SWEEP pass: force latch 0x00..0xff, 60 render frames each ==\n");
    g_phase = 1;
    for (int cmd = 0x00; cmd <= 0xff; cmd++) {
        g_cmd = cmd;
        shinobi_audio_command((uint8_t)cmd);
        for (int fr = 0; fr < 60; fr++) {
            g_frame = fr;
            guarded_render(buf, SH_SPF);
        }
        if ((cmd & 0x1f) == 0x1f)
            fprintf(stderr, "  swept up to cmd 0x%02x (smpW=%u ymW=%u)\n",
                    cmd, shinobi_sound_dbg_sample_writes(), shinobi_sound_dbg_ym_writes());
    }

    fprintf(stderr, "== ALL PASSES CLEAN: no ASan/UBSan trap, no watchdog hang ==\n");
    free(buf);
    return 0;
}
