/* shdancer_render.c -- HOST (Linux, native cc) headless render + diagnostic
 * harness for the Shadow Dancer Amiga port.
 *
 * It drives the port's OWN engine and renderer unchanged:
 *   - hal/shinobi_interp.c   : Musashi 68000 interpreter + System-18 memory map
 *   - hal/shinobi_hwrender.c : the -DSHINOBI_RTG software compositor
 * and dumps the resulting chunky pen frame (320x224) through the reduced 256-pen
 * palette to PPM files so the video can be inspected/iterated without amiberry.
 *
 * usage: shdancer_render [frames=600] [roms_dir]
 *   env SHDANCER_ROMS overrides the ROM directory too.
 * Outputs /tmp/shd_host_<fr>.ppm at frames 200/400/600 (+ the final frame).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "m68k.h"

/* engine entry points (hal/shinobi_interp.c) */
extern int  shinobi_dyntrans_init(void);
extern void shinobi_dyntrans_frame(void);
extern void shinobi_dyntrans_set_inputs(uint8_t p1, uint8_t p2, uint8_t svc,
                                        uint8_t d1, uint8_t d2);
extern int  shinobi_dyntrans_rendered(void);

/* renderer entry points (hal/shinobi_hwrender.c) */
extern void shinobi_hw_open(void);
extern const unsigned char *shinobi_chunky(void);
extern const unsigned char *shinobi_pal256(int *npens);
extern void shinobi_dims(int *w, int *h);
extern void shinobi_diag_get(unsigned *calls, unsigned *last,
                             unsigned char banks_out[8], int *sprite_entries);

/* host assets (tools/shdancer_assets_host.c) */
extern int         shinobi_assets_load(void);
extern const char *shinobi_assets_error(void);
extern void        shinobi_assets_host_set_dir(const char *dir);

static void write_ppm(int fr)
{
    int w, h;
    shinobi_dims(&w, &h);
    const unsigned char *chunky = shinobi_chunky();
    int npens = 0;
    const unsigned char *pal = shinobi_pal256(&npens);

    char path[256];
    snprintf(path, sizeof path, "/tmp/shd_host_%d.ppm", fr);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned pen = chunky[y * w + x];
            const unsigned char *c = pal + (size_t)pen * 3u;
            fputc(c[0], f);
            fputc(c[1], f);
            fputc(c[2], f);
        }
    }
    fclose(f);
    fprintf(stderr, "wrote %s (%dx%d, npens=%d)\n", path, w, h, npens);
}

static void report(int fr)
{
    unsigned calls = 0, last = 0;
    unsigned char banks[8] = {0};
    int spr = -1;
    shinobi_diag_get(&calls, &last, banks, &spr);
    unsigned pc = m68k_get_reg(NULL, M68K_REG_PC);
    fprintf(stderr,
        "[fr %4d] PC=%06x  tilebank8: writes=%u last=%02x  banks=[%d %d %d %d %d %d %d %d]  sprite_entries=%d\n",
        fr, pc, calls, last,
        banks[0], banks[1], banks[2], banks[3],
        banks[4], banks[5], banks[6], banks[7], spr);
}

int main(int argc, char **argv)
{
    int frames = (argc > 1) ? atoi(argv[1]) : 600;
    if (frames <= 0) frames = 600;

    const char *romdir = (argc > 2) ? argv[2] : getenv("SHDANCER_ROMS");
    if (romdir && romdir[0]) shinobi_assets_host_set_dir(romdir);

    if (!shinobi_dyntrans_init()) {
        fprintf(stderr, "shinobi_dyntrans_init failed: %s\n", shinobi_assets_error());
        return 1;
    }
    /* -DSHINOBI_RTG: shinobi_hw_open() just arms the software compositor (s_ok=1)
     * so shinobi_render() actually paints s_native. */
    shinobi_hw_open();
    shinobi_dyntrans_set_inputs(0xff, 0xff, 0xff, 0xff, 0xff);   /* all idle */

    unsigned pc0 = m68k_get_reg(NULL, M68K_REG_PC);
    fprintf(stderr, "init ok. reset/entry PC=%06x. running %d frames (idle inputs)...\n",
            pc0, frames);

    for (int fr = 1; fr <= frames; fr++) {
        /* Insert a coin then press start to reach gameplay (active-low). Standard
         * Sega System-16B: service port bit0=coin1, bit4=start1. Try several bits
         * since the exact map varies; harmless if a bit isn't the real one. */
        uint8_t svc = 0xff, p1 = 0xff;
        if (fr >= 100 && fr < 108) svc &= ~0x01;            /* coin1 */
        if (fr >= 160 && fr < 168) { svc &= ~0x10; p1 &= ~0x80; } /* start1 */
        shinobi_dyntrans_set_inputs(p1, 0xff, svc, 0xff, 0xff);

        shinobi_dyntrans_frame();

        int checkpoint = (fr == 200 || fr == 400 || fr == 600 || fr == frames);
        if (checkpoint) {
            /* the interp renders on every other frame; make sure s_native holds
             * this frame's image before we snapshot it. */
            if (!shinobi_dyntrans_rendered())
                shinobi_dyntrans_frame();
            report(fr);
            write_ppm(fr);
        }
    }

    fprintf(stderr, "done.\n");
    return 0;
}
