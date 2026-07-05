/* aliensyn_render.c -- HOST (Linux, native cc) headless render + diagnostic
 * harness for the Alien Syndrome (Sega System 16B, aliensyn) Amiga port.
 *
 * It drives the port's OWN engine and renderer unchanged:
 *   - hal/shinobi_interp.c   : Musashi 68000 interpreter + System-16B memory map
 *   - hal/shinobi_hwrender.c : the -DSHINOBI_RTG software compositor
 * and dumps the resulting chunky pen frame (320x224) through the reduced 256-pen
 * palette to PPM files so the video can be inspected without amiberry. It ALSO
 * traces the guest PC at each checkpoint and scans every rendered frame for
 * "went black" (uniform frame) and "hang" (identical successive frames) so we
 * can pin the exact frame/PC where the port dies on device.
 *
 * usage: aliensyn_render [frames=1200] [roms_dir]
 *   env ALIENSYN_ROMS  overrides the ROM directory.
 *   env ALIENSYN_COIN  "1"(default)=inject coin+start around fr 100/160,
 *                      "0"=idle inputs the whole run.
 *   env ALIENSYN_TAG   output tag (e.g. "idle"/"coin"); PPMs go to
 *                      /tmp/asyn_host[_<tag>]_<fr>.ppm.
 * Outputs PPM at frames 100/300/600/900/1200 (+ the final frame).
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
/* NOTE: no shinobi_diag_get() in the 16B Alien port (tile banking is trivial);
 * PC + render frames are what matter here. */

/* host assets (tools/aliensyn_assets_host.c) */
extern int         shinobi_assets_load(void);
extern const char *shinobi_assets_error(void);
extern void        shinobi_assets_host_set_dir(const char *dir);

static char s_tag[64] = "";

static void ppm_path(char *buf, size_t n, int fr)
{
    if (s_tag[0])
        snprintf(buf, n, "/tmp/asyn_host_%s_%d.ppm", s_tag, fr);
    else
        snprintf(buf, n, "/tmp/asyn_host_%d.ppm", fr);
}

static void write_ppm(int fr)
{
    int w, h;
    shinobi_dims(&w, &h);
    const unsigned char *chunky = shinobi_chunky();
    int npens = 0;
    const unsigned char *pal = shinobi_pal256(&npens);

    char path[256];
    ppm_path(path, sizeof path, fr);
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

/* Scan the current chunky frame: count pixels that differ from the top-left pen
 * (a cheap "how much non-background content is on screen" measure), the number
 * of distinct pens, and a rolling hash for hang detection. Returns diff count. */
static unsigned scan_frame(unsigned *distinct_out, unsigned *hash_out, unsigned *pen0_out)
{
    int w, h;
    shinobi_dims(&w, &h);
    const unsigned char *c = shinobi_chunky();
    unsigned pen0 = c[0];
    unsigned diff = 0, hash = 2166136261u;
    unsigned char seen[256] = {0};
    unsigned distinct = 0;
    for (int i = 0; i < w * h; i++) {
        unsigned p = c[i];
        if (p != pen0) diff++;
        if (!seen[p]) { seen[p] = 1; distinct++; }
        hash = (hash ^ p) * 16777619u;
    }
    if (distinct_out) *distinct_out = distinct;
    if (hash_out) *hash_out = hash;
    if (pen0_out) *pen0_out = pen0;
    return diff;
}

static unsigned pc_at(void) { return m68k_get_reg(NULL, M68K_REG_PC); }

static void report(int fr, unsigned prev_pc)
{
    unsigned pc = pc_at();
    unsigned distinct = 0, hash = 0, pen0 = 0;
    unsigned diff = scan_frame(&distinct, &hash, &pen0);
    const char *pcflag = "";
    if (pc >= 0x30000 && pc < 0x80000)
        pcflag = "  <<< PC IN ZERO-PAD PROGRAM WINDOW (crash into bad memory)";
    else if (pc == prev_pc)
        pcflag = "  <<< PC UNCHANGED SINCE LAST CHECKPOINT (possible hang)";
    fprintf(stderr,
        "[fr %4d] PC=%06x  frame: nonbg=%u distinct_pens=%u pen0=%u hash=%08x%s\n",
        fr, pc, diff, distinct, pen0, hash, pcflag);
}

int main(int argc, char **argv)
{
    int frames = (argc > 1) ? atoi(argv[1]) : 1200;
    if (frames <= 0) frames = 1200;

    const char *romdir = (argc > 2) ? argv[2] : getenv("ALIENSYN_ROMS");
    if (romdir && romdir[0]) shinobi_assets_host_set_dir(romdir);

    const char *tag = getenv("ALIENSYN_TAG");
    if (tag && tag[0]) { strncpy(s_tag, tag, sizeof s_tag - 1); s_tag[sizeof s_tag - 1] = 0; }

    int inject_coin = 1;
    const char *coin_env = getenv("ALIENSYN_COIN");
    if (coin_env && coin_env[0] == '0') inject_coin = 0;

    if (!shinobi_dyntrans_init()) {
        fprintf(stderr, "shinobi_dyntrans_init failed: %s\n", shinobi_assets_error());
        return 1;
    }
    /* Optional: disassemble guest code and exit (ALIENSYN_DISASM="start:count").
     * Diagnostic only; uses the interp's m68k_read_disassembler_* over guest RAM. */
    const char *dis = getenv("ALIENSYN_DISASM");
    if (dis && dis[0]) {
        unsigned start = 0, count = 40;
        sscanf(dis, "%x:%u", &start, &count);
        unsigned pc = start;
        for (unsigned i = 0; i < count; i++) {
            char buf[128];
            unsigned len = m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
            fprintf(stderr, "%06x: %s\n", pc, buf);
            pc += len ? len : 2;
        }
        return 0;
    }

    /* -DSHINOBI_RTG: shinobi_hw_open() just arms the software compositor (s_ok=1)
     * so shinobi_render() actually paints s_native. */
    shinobi_hw_open();
    shinobi_dyntrans_set_inputs(0xff, 0xff, 0xff, 0xff, 0xff);   /* all idle */

    /* Optional: hexdump guest memory after init (ALIENSYN_MEMDUMP="addr:len"). */
    const char *md = getenv("ALIENSYN_MEMDUMP");
    if (md && md[0]) {
        unsigned a = 0, n = 32; sscanf(md, "%x:%u", &a, &n);
        for (unsigned i = 0; i < n; i += 16) {
            fprintf(stderr, "%06x:", a + i);
            for (unsigned j = 0; j < 16 && i + j < n; j++)
                fprintf(stderr, " %02x", m68k_read_memory_8(a + i + j));
            fprintf(stderr, "\n");
        }
        return 0;
    }

    /* Optional DIAGNOSIS-CONFIRM test (tools-only, no port logic changed): the
     * aliensyn boot ROM writes its 16-byte 315-5195 region-config table to
     * 0xC00020, which this interp does not decode (it only accepts mapper writes
     * at 0xFE0020+). Re-route that same table through the legitimate 0xFE0020
     * path and see whether the derail disappears and the port renders.
     * (ALIENSYN_MAPFIX=1) */
    if (getenv("ALIENSYN_MAPFIX")) {
        /* config table lives at guest 0x19e0 (copied to 0xc00020 by boot) */
        for (unsigned r = 0; r < 16; r++) {
            unsigned b = m68k_read_memory_8(0x19e0 + r);   /* one config byte  */
            m68k_write_memory_8(0xfe0020 + r * 2 + 1, b);  /* -> mapper reg 0x10+r */
        }
        fprintf(stderr, "MAPFIX: programmed 16 region regs from guest 0x19e0 via 0xfe0020\n");
    }

    /* Optional: run the real (IRQ-driven) frame loop but single-step, watching
     * for the PC to derail out of the executable program/RAM. On the first bad
     * fetch, dump a ring buffer of the last instructions that led there.
     * (ALIENSYN_DERAIL=1) */
    if (getenv("ALIENSYN_DERAIL")) {
        enum { RING = 48 };
        unsigned rpc[RING]; char rds[RING][96];
        int rn = 0; long total = 0; long cap = 30L * 1000 * 1000;
        int found = 0;
        while (total < cap && !found) {
            m68k_set_irq(4);
            int budget = 166666;
            while (budget > 0) {
                unsigned pc = pc_at();
                /* valid execute regions: ROM/low prog 0..0x40000, work RAM the
                 * game runs from is inside that mirror; anything else = derail. */
                int bad = (pc >= 0x40000);
                char buf[96];
                m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
                rpc[rn % RING] = pc;
                strncpy(rds[rn % RING], buf, sizeof rds[0] - 1);
                rds[rn % RING][sizeof rds[0]-1] = 0;
                rn++;
                if (bad) {
                    fprintf(stderr, "*** DERAIL: PC=%06x after %ld steps. last %d instrs:\n",
                            pc, total, RING);
                    for (int k = 0; k < RING && k < rn; k++) {
                        int idx = (rn - RING + k);
                        if (idx < 0) continue;
                        fprintf(stderr, "   %06x: %s\n", rpc[idx % RING], rds[idx % RING]);
                    }
                    found = 1; break;
                }
                int used = m68k_execute(1);
                budget -= used > 0 ? used : 1;
                total++;
                if (total >= cap) break;
            }
            m68k_set_irq(0);
        }
        if (!found) fprintf(stderr, "no derail in %ld steps\n", total);
        return 0;
    }

    /* Optional: warm up N frames, then single-step M instructions printing the
     * PC trace so we can see the exact steady-state loop (ALIENSYN_STEPTRACE="N:M"). */
    const char *st = getenv("ALIENSYN_STEPTRACE");
    if (st && st[0]) {
        unsigned warm = 50, steps = 200;
        sscanf(st, "%u:%u", &warm, &steps);
        for (unsigned f = 0; f < warm; f++) shinobi_dyntrans_frame();
        fprintf(stderr, "STEPTRACE after %u frames, %u single steps:\n", warm, steps);
        for (unsigned i = 0; i < steps; i++) {
            unsigned pc = pc_at();
            char buf[128];
            m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
            unsigned sr = m68k_get_reg(NULL, M68K_REG_SR);
            fprintf(stderr, "  %06x sr=%04x: %s\n", pc, sr, buf);
            m68k_execute(1);
        }
        return 0;
    }

    unsigned pc0 = pc_at();
    fprintf(stderr, "init ok [tag='%s' coin=%s]. reset/entry PC=%06x. running %d frames...\n",
            s_tag, inject_coin ? "on" : "off", pc0, frames);

    /* per-frame blackness / hang tracking */
    int first_blank_fr = -1;      /* first rendered frame that became uniform (all one pen) */
    int had_content    = 0;       /* have we seen a non-uniform frame yet? */
    int blank_since     = -1;     /* run start of a current all-blank streak */
    unsigned last_hash = 0;
    int hang_run = 0, hang_reported = 0;
    unsigned checkpoint_pc = pc0;

    const int cps[] = { 100, 300, 600, 900, 1200 };
    const int ncps = (int)(sizeof cps / sizeof cps[0]);

    for (int fr = 1; fr <= frames; fr++) {
        /* Insert a coin then press start to reach gameplay (active-low). Standard
         * Sega System-16B service port: bit0=coin1, bit4=start1 (task-specified). */
        uint8_t svc = 0xff, p1 = 0xff;
        if (inject_coin) {
            if (fr >= 100 && fr < 108) svc &= ~0x01;              /* coin1  */
            if (fr >= 160 && fr < 168) { svc &= ~0x10; p1 &= ~0x80; } /* start1 */
        }
        shinobi_dyntrans_set_inputs(p1, 0xff, svc, 0xff, 0xff);

        shinobi_dyntrans_frame();

        /* Track content/blank per rendered frame (interp renders every other frame). */
        if (shinobi_dyntrans_rendered()) {
            unsigned distinct = 0, hash = 0, pen0 = 0;
            (void)scan_frame(&distinct, &hash, &pen0);
            int uniform = (distinct <= 1);
            if (!uniform) { had_content = 1; blank_since = -1; }
            else if (blank_since < 0) blank_since = fr;
            /* first frame that goes blank AFTER real content has been drawn */
            if (uniform && had_content && first_blank_fr < 0) {
                first_blank_fr = fr;
                fprintf(stderr,
                    "[fr %4d] *** FRAME WENT BLANK (uniform pen %u) *** PC=%06x\n",
                    fr, pen0, pc_at());
            }
            /* hang: many identical rendered frames in a row */
            if (hash == last_hash) hang_run++;
            else { hang_run = 0; last_hash = hash; }
            if (hang_run == 120 && !hang_reported) {
                hang_reported = 1;
                fprintf(stderr,
                    "[fr %4d] *** ~120 identical rendered frames (static/hung image) *** PC=%06x\n",
                    fr, pc_at());
            }
        }

        int is_cp = 0;
        for (int i = 0; i < ncps; i++) if (cps[i] == fr) is_cp = 1;
        if (fr == frames) is_cp = 1;
        if (is_cp) {
            /* the interp renders on every other frame; make sure s_native holds
             * this frame's image before we snapshot it. */
            if (!shinobi_dyntrans_rendered())
                shinobi_dyntrans_frame();
            report(fr, checkpoint_pc);
            checkpoint_pc = pc_at();
            write_ppm(fr);
        }
    }

    fprintf(stderr, "---- summary [tag='%s' coin=%s] ----\n", s_tag, inject_coin ? "on" : "off");
    if (first_blank_fr >= 0)
        fprintf(stderr, "render went BLANK at frame %d (uniform single-pen frame)\n", first_blank_fr);
    else
        fprintf(stderr, "render never went fully blank across %d frames (content kept updating)\n", frames);
    if (blank_since >= 0 && first_blank_fr < 0 && !had_content)
        fprintf(stderr, "NOTE: frame was uniform from the very start (never rendered content)\n");
    fprintf(stderr, "done.\n");
    return 0;
}
