#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ymfm.h"
#include "ymfm_opm.h"

#define SH_OUT_RATE     11025
#define SH_YM2151_CLOCK 4000000

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
}

namespace {

struct sh_ym_intf : public ymfm::ymfm_interface {
    void ymfm_set_timer(uint32_t, int32_t) override { }
    void ymfm_update_irq(bool) override { }
};

static sh_ym_intf intf;
static ymfm::ym2151 chip(intf);

static int load_file(const char *path, uint8_t *dst, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t got = fread(dst, 1, n, f);
    fclose(f);
    return got == n;
}

static void wav_header(FILE *f, uint32_t samples)
{
    uint32_t data = samples;
    uint32_t riff = 36 + data;
    uint32_t rate = SH_OUT_RATE;
    uint16_t one = 1;
    uint16_t bits = 8;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmt = 16;
    fwrite(&fmt, 4, 1, f);
    fwrite(&one, 2, 1, f);
    fwrite(&one, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&one, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
}

}

extern "C" {

void shinobi_ym2151_reset(void)
{
    chip.reset();
}

void shinobi_ym2151_write_addr(uint8_t v)
{
    chip.write_address(v);
}

void shinobi_ym2151_write_data(uint8_t v)
{
    chip.write_data(v);
}

uint8_t shinobi_ym2151_read_status(void)
{
    return 0;
}

int shinobi_ym2151_sample(void)
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
        if (i >= count - 2) {
            sum += (out.data[0] + out.data[1]) >> 1;
            navg++;
        }
    }
    int s = (int)(sum / navg);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return s;
}

}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s epr-11361.a10 mpr-11362.a11 [cmd...]\n", argv[0]);
        return 2;
    }

    shinobi_rom_sound = (uint8_t*)calloc(1, 0x8000);
    shinobi_rom_sample = (uint8_t*)calloc(1, 0x20000);
    if (!shinobi_rom_sound || !shinobi_rom_sample ||
        !load_file(argv[1], shinobi_rom_sound, 0x8000) ||
        !load_file(argv[2], shinobi_rom_sample, 0x20000)) {
        fprintf(stderr, "failed to load sound ROMs\n");
        return 1;
    }

    shinobi_sound_init();
    signed char buf[512];
    FILE *wav = 0;
    uint32_t wav_samples = 0;
    if (const char *path = getenv("SHINOBI_WAV")) {
        wav = fopen(path, "wb+");
        if (wav)
            wav_header(wav, 0);
    }
    int peak = 0;
    unsigned long sum_abs = 0;
    unsigned long samples = 0;

    for (int i = 0; i < 60; i++) {
        shinobi_sound_render(buf, sizeof buf);
        if (wav) {
            for (unsigned j = 0; j < sizeof buf; j++) {
                uint8_t u = (uint8_t)((int)buf[j] + 128);
                fwrite(&u, 1, 1, wav);
            }
            wav_samples += sizeof buf;
        }
    }

    static const unsigned default_cmds[] = { 0x9a, 0xa4, 0xa7, 0xb3, 0xb8 };
    int cmd_count = argc > 3 ? argc - 3 : (int)(sizeof default_cmds / sizeof default_cmds[0]);
    for (int ci = 0; ci < cmd_count; ci++) {
        unsigned cmd = argc > 3 ? (unsigned)strtoul(argv[3 + ci], 0, 0) : default_cmds[ci];
        shinobi_audio_command((uint8_t)cmd);
        for (int i = 0; i < 60; i++) {
            shinobi_sound_render(buf, sizeof buf);
            if (wav) {
                for (unsigned j = 0; j < sizeof buf; j++) {
                    uint8_t u = (uint8_t)((int)buf[j] + 128);
                    fwrite(&u, 1, 1, wav);
                }
                wav_samples += sizeof buf;
            }
            for (unsigned j = 0; j < sizeof buf; j++) {
                int v = buf[j];
                int a = v < 0 ? -v : v;
                if (a > peak) peak = a;
                sum_abs += (unsigned long)a;
                samples++;
            }
        }
    }

    if (wav) {
        fseek(wav, 0, SEEK_SET);
        wav_header(wav, wav_samples);
        fclose(wav);
    }

    printf("cmds=%u high=%u ym=%u keyons=%u sample_w=%u pcm_nonzero=%u peak=%d mean_abs=%lu\n",
           shinobi_sound_dbg_commands(),
           shinobi_sound_dbg_high_commands(),
           shinobi_sound_dbg_ym_writes(),
           shinobi_sound_dbg_keyons(),
           shinobi_sound_dbg_sample_writes(),
           shinobi_sound_dbg_pcm_nonzero(),
           peak,
           samples ? sum_abs / samples : 0);
    if (shinobi_sound_dbg_sample_writes()) {
        printf("sample_bytes:");
        for (unsigned i = 0; i < 64 && i < shinobi_sound_dbg_sample_writes(); i++)
            printf(" %02x", shinobi_sound_dbg_sample_byte(i));
        printf("\n");
    }
    return 0;
}
