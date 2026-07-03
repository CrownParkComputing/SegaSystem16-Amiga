/* Host harness: exercise the REAL Shinobi port audio path
 * (shinobi_sound.c interp build, SHINOBI_AUDIO_RUST=0) and dump a WAV at the
 * port's output rate (8040 Hz). Replaces only the YM2151 C-interface and the
 * ROM loaders; the Z80, uPD7759 handshake, direct_pcm decode and FM/PCM mix
 * are the exact code shipped to the Amiga. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ymfm.h"
#include "ymfm_opm.h"

extern "C" {
uint8_t *shinobi_rom_main;
uint8_t *shinobi_gfx_tp0;
uint8_t *shinobi_gfx_tp1;
uint8_t *shinobi_gfx_tp2;
uint8_t *shinobi_gfx_spr;
uint8_t *shinobi_rom_sound;
uint8_t *shinobi_rom_sample;

void shinobi_sound_init(void);
void shinobi_audio_command(uint8_t v);
void shinobi_sound_render(signed char *out, int n);
unsigned shinobi_sound_dbg_ym_writes(void);
unsigned shinobi_sound_dbg_keyons(void);
unsigned shinobi_sound_dbg_sample_writes(void);
unsigned shinobi_sound_dbg_sample_byte(unsigned i);
unsigned shinobi_sound_dbg_commands(void);
unsigned shinobi_sound_dbg_high_commands(void);
unsigned shinobi_sound_dbg_pcm_nonzero(void);

/* YM2151 C-interface expected by shinobi_sound.c — backed by ymfm at 8040 Hz,
 * matching the shipped shinobi_ym2151.cpp (not the probe's 11025 rate). */
void shinobi_ym2151_reset(void);
void shinobi_ym2151_write_addr(uint8_t v);
void shinobi_ym2151_write_data(uint8_t v);
uint8_t shinobi_ym2151_read_status(void);
int shinobi_ym2151_sample(void);
}

namespace {
struct sh_ym_intf : public ymfm::ymfm_interface {
    void ymfm_set_timer(uint32_t, int32_t) override { }
    void ymfm_update_irq(bool) override { }
};
static sh_ym_intf intf;
static ymfm::ym2151 chip(intf);
static uint8_t ym_addr;

static int load_file(const char *p, uint8_t *dst, size_t n) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    size_t g = fread(dst, 1, n, f);
    fclose(f);
    return g == n;
}
}

#define SH_OUT_RATE     8040
#define SH_YM2151_CLOCK 4000000

extern "C" void shinobi_ym2151_reset(void) { chip.reset(); ym_addr = 0; }
extern "C" void shinobi_ym2151_write_addr(uint8_t v) { chip.write_address(v); ym_addr = v; }
extern "C" void shinobi_ym2151_write_data(uint8_t v) { chip.write_data(v); }
extern "C" uint8_t shinobi_ym2151_read_status(void) { return 0; }

extern "C" int shinobi_ym2151_sample(void)
{
    static uint32_t step = 0, frac = 0;
    if (step == 0)
        step = (uint32_t)(((uint64_t)(SH_YM2151_CLOCK / 64) << 16) / (uint32_t)SH_OUT_RATE);
    frac += step;
    int count = (int)(frac >> 16);
    frac &= 0xffff;
    if (count < 1) count = 1;
    ymfm::ym2151::output_data out;
    long sum = 0;
    int navg = 0;
    for (int i = 0; i < count; i++) {
        chip.generate(&out, 1);
        if (i >= count - 2) { sum += (out.data[0] + out.data[1]) >> 1; navg++; }
    }
    int s = (int)(sum / (navg ? navg : 1));
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return s;
}

static void wav_header(FILE *f, uint32_t samples) {
    uint32_t data = samples;
    uint32_t riff = 36 + data;
    uint32_t rate = SH_OUT_RATE;
    uint16_t one = 1, bits = 8;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmt = 16; fwrite(&fmt, 4, 1, f);
    fwrite(&one, 2, 1, f); fwrite(&one, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&one, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s epr-11361.a10 mpr-11362.a11 CMD [seconds] [out.wav]\n", argv[0]);
        return 2;
    }
    shinobi_rom_sound = (uint8_t*)calloc(1, 0x8000);
    shinobi_rom_sample = (uint8_t*)calloc(1, 0x20000);
    if (!load_file(argv[1], shinobi_rom_sound, 0x8000) ||
        !load_file(argv[2], shinobi_rom_sample, 0x20000)) {
        fprintf(stderr, "failed to load sound ROMs\n");
        return 1;
    }
    signed char buf[256];
    if (strcmp(argv[3], "sweep") == 0) {
        double secs = argc > 4 ? atof(argv[4]) : 2.0;
        int total = (int)(secs * SH_OUT_RATE);
        printf("cmd  peak0-100ms  peakRest  keyons smpwr  (<<< = loud t0 burst)\n");
        for (unsigned cmd = 0; cmd < 0x100; cmd++) {
            shinobi_sound_init();
            int bf = (int)(0.8 * SH_OUT_RATE / sizeof buf) + 1;
            for (int i = 0; i < bf; i++) shinobi_sound_render(buf, sizeof buf);
            unsigned k0 = shinobi_sound_dbg_keyons(), s0 = shinobi_sound_dbg_sample_writes();
            shinobi_audio_command((uint8_t)cmd);
            int rendered = 0, p0 = 0, prest = 0, n0 = 100 * SH_OUT_RATE / 1000;
            for (; rendered < total; ) {
                int n = total - rendered; if (n > (int)sizeof buf) n = sizeof buf;
                shinobi_sound_render(buf, n);
                for (int j = 0; j < n; j++) {
                    int a = buf[j] < 0 ? -buf[j] : buf[j];
                    if (rendered + j < n0) { if (a > p0) p0 = a; }
                    else { if (a > prest) prest = a; }
                }
                rendered += n;
            }
            unsigned k = shinobi_sound_dbg_keyons() - k0; unsigned pcm = shinobi_sound_dbg_pcm_nonzero();
            unsigned s = shinobi_sound_dbg_sample_writes() - s0;
            if (p0 >= 30 || prest >= 40 || s || k > 2 || pcm > 0)
                printf("0x%02x  %9d  %8d   %4u   %4u  pcm=%u %s\n",
                       cmd, p0, prest, k, s, p0 >= 40 ? "<<<" : "");
        }
        return 0;
    }

    unsigned cmd = (unsigned)strtoul(argv[3], 0, 0);
    double secs = argc > 4 ? atof(argv[4]) : 4.0;
    const char *wavpath = argc > 5 ? argv[5] : 0;

    shinobi_sound_init();

    /* boot: run ~0.8s of audio first so the sound program is settled. */
    int boot_frames = (int)(0.8 * SH_OUT_RATE / sizeof buf) + 1;
    for (int i = 0; i < boot_frames; i++) shinobi_sound_render(buf, sizeof buf);

    FILE *wav = 0; uint32_t wav_n = 0;
    if (wavpath) { wav = fopen(wavpath, "wb+"); if (wav) wav_header(wav, 0); }

    /* mark time zero at the command latch. */
    printf("=== sending command 0x%02x, capture %.2fs ===\n", cmd, secs);
    shinobi_audio_command((uint8_t)cmd);

    int total = (int)(secs * SH_OUT_RATE);
    int rendered = 0;
    /* envelope: peak abs per 10ms bucket */
    int bucket = 0, bucket_max = 0; int bucket_idx = 0;
    for (; rendered < total; ) {
        int n = total - rendered; if (n > (int)sizeof buf) n = sizeof buf;
        shinobi_sound_render(buf, n);
        if (wav) {
            for (int j = 0; j < n; j++) { uint8_t u = (uint8_t)((int)buf[j] + 128); fwrite(&u, 1, 1, wav); }
            wav_n += n;
        }
        for (int j = 0; j < n; j++) {
            int a = buf[j] < 0 ? -buf[j] : buf[j];
            if (a > bucket_max) bucket_max = a;
            if (++bucket >= 80) { /* 80 samples ~ 10ms @ 8040 */
                printf("  t=%5.2fs peak=%3d %s\n", bucket_idx * 0.010, bucket_max,
                       bucket_max > 40 ? "<<<" : "");
                bucket_idx++; bucket = 0; bucket_max = 0;
            }
        }
        rendered += n;
    }
    if (bucket) printf("  t=%5.2fs peak=%3d (partial)\n", bucket_idx * 0.010, bucket_max);

    if (wav) { fseek(wav, 0, SEEK_SET); wav_header(wav, wav_n); fclose(wav); }

    printf("=== dbg: cmds=%u high=%u ym_writes=%u keyons=%u sample_writes=%u pcm_nonzero=%u\n",
           shinobi_sound_dbg_commands(), shinobi_sound_dbg_high_commands(),
           shinobi_sound_dbg_ym_writes(), shinobi_sound_dbg_keyons(),
           shinobi_sound_dbg_sample_writes(), shinobi_sound_dbg_pcm_nonzero());
    if (shinobi_sound_dbg_sample_writes()) {
        printf("sample_bytes:");
        for (unsigned i = 0; i < 64 && i < shinobi_sound_dbg_sample_writes(); i++)
            printf(" %02x", shinobi_sound_dbg_sample_byte(i));
        printf("\n");
    }
    return 0;
}