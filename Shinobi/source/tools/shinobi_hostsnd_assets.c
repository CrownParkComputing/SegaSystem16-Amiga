/* shinobi_hostsnd_assets.c -- HOST (Linux native cc) replacement for the port's
 * hal/shinobi_assets.c, loading the shinobi6 (Sega System 16B, unprotected set 6
 * on ROM board 171-5521) ROM set from a normal host directory. Reproduces the
 * EXACT interleave/offsets/sizes used on target by hal/shinobi_assets.c so the
 * sound path (epr-11361.a10 program, mpr-11362.a11 samples) is driven with
 * byte-identical data. Only difference: stdio fopen/fread instead of AmigaDOS.
 *
 * Layout mirror of hal/shinobi_assets.c load_set_from()/try_load_sound_from():
 *   main   0x80000 alloc: two 0x20000 program ROMs ROM_LOAD16_BYTE (stride 2):
 *            epr-11360.a7 -> even bytes (offset 0), epr-11359.a5 -> odd (offset 1)
 *          fills 0..0x3ffff, then the 0x40000 program is mirrored into 0x40000..0x7ffff
 *   tiles  3 planes 0x20000 each, sequential:
 *            mpr-11363.a14 -> tp0, mpr-11364.a15 -> tp1, mpr-11365.a16 -> tp2
 *   spr    0x80000 region, 0x20000 strided halves:
 *            mpr-11368.b5 @0x00000, mpr-11366.b1 @0x00001,
 *            mpr-11369.b6 @0x40000, mpr-11367.b2 @0x40001
 *   sound  0x8000 : epr-11361.a10 -> shinobi_rom_sound
 *   sample 0x20000: mpr-11362.a11 -> shinobi_rom_sample
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *shinobi_rom_main;
uint8_t *shinobi_gfx_tp0;
uint8_t *shinobi_gfx_tp1;
uint8_t *shinobi_gfx_tp2;
uint8_t *shinobi_gfx_spr;
uint8_t *shinobi_rom_sound;
uint8_t *shinobi_rom_sample;

#define MAIN_HALF_SIZE  0x20000u   /* per program ROM, and per tile/spr/sample ROM */
#define TILE_PLANE_SIZE 0x20000u
#define MAIN_SIZE       0x40000u
#define MAIN_ALLOC_SIZE 0x80000u
#define SPR_REGION_SIZE 0x80000u
#define SOUND_ROM_SIZE  0x8000u
#define SAMPLE_ROM_SIZE 0x20000u

static char s_error[256];
static char s_dir[1024] = "/home/jon/SegaSystem16-Amiga/Shinobi/roms/shinobi6";

void shinobi_assets_host_set_dir(const char *dir)
{
    if (dir && dir[0]) {
        strncpy(s_dir, dir, sizeof s_dir - 1);
        s_dir[sizeof s_dir - 1] = 0;
    }
}

const char *shinobi_assets_error(void)
{
    return s_error[0] ? s_error : "missing Shinobi ROM assets";
}

static int read_exact(const char *fname, uint8_t *dst, uint32_t bytes)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", s_dir, fname);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(s_error, sizeof s_error, "cannot open %s", path); return 0; }
    size_t got = fread(dst, 1, bytes, f);
    fclose(f);
    if (got != bytes) {
        snprintf(s_error, sizeof s_error, "short read %s (%zu/%u)", path, got, bytes);
        return 0;
    }
    return 1;
}

/* src[i] -> dst[offset + i*2], i < MAIN_HALF_SIZE (ROM_LOAD16_BYTE even/odd). */
static int read_strided(const char *fname, uint8_t *dst, uint32_t offset)
{
    uint8_t *tmp = (uint8_t *)malloc(MAIN_HALF_SIZE);
    if (!tmp) { snprintf(s_error, sizeof s_error, "oom import buffer"); return 0; }
    if (!read_exact(fname, tmp, MAIN_HALF_SIZE)) { free(tmp); return 0; }
    for (uint32_t i = 0; i < MAIN_HALF_SIZE; i++)
        dst[offset + i * 2u] = tmp[i];
    free(tmp);
    return 1;
}

int shinobi_assets_load(void)
{
    if (shinobi_rom_main && shinobi_gfx_tp0 && shinobi_gfx_tp1 &&
        shinobi_gfx_tp2 && shinobi_gfx_spr)
        return 1;

    shinobi_rom_main   = (uint8_t *)calloc(1, MAIN_ALLOC_SIZE);
    shinobi_gfx_tp0    = (uint8_t *)calloc(1, TILE_PLANE_SIZE);
    shinobi_gfx_tp1    = (uint8_t *)calloc(1, TILE_PLANE_SIZE);
    shinobi_gfx_tp2    = (uint8_t *)calloc(1, TILE_PLANE_SIZE);
    shinobi_gfx_spr    = (uint8_t *)calloc(1, SPR_REGION_SIZE);
    shinobi_rom_sound  = (uint8_t *)calloc(1, SOUND_ROM_SIZE);
    shinobi_rom_sample = (uint8_t *)calloc(1, SAMPLE_ROM_SIZE);
    if (!shinobi_rom_main || !shinobi_gfx_tp0 || !shinobi_gfx_tp1 ||
        !shinobi_gfx_tp2 || !shinobi_gfx_spr ||
        !shinobi_rom_sound || !shinobi_rom_sample) {
        snprintf(s_error, sizeof s_error, "not enough memory for ROM assets");
        return 0;
    }

    if (!read_strided("epr-11360.a7", shinobi_rom_main, 0)) return 0;
    if (!read_strided("epr-11359.a5", shinobi_rom_main, 1)) return 0;

    if (!read_exact("mpr-11363.a14", shinobi_gfx_tp0, TILE_PLANE_SIZE)) return 0;
    if (!read_exact("mpr-11364.a15", shinobi_gfx_tp1, TILE_PLANE_SIZE)) return 0;
    if (!read_exact("mpr-11365.a16", shinobi_gfx_tp2, TILE_PLANE_SIZE)) return 0;

    if (!read_strided("mpr-11368.b5", shinobi_gfx_spr, 0x00000)) return 0;
    if (!read_strided("mpr-11366.b1", shinobi_gfx_spr, 0x00001)) return 0;
    if (!read_strided("mpr-11369.b6", shinobi_gfx_spr, 0x40000)) return 0;
    if (!read_strided("mpr-11367.b2", shinobi_gfx_spr, 0x40001)) return 0;

    /* Mirror the 256KB program into the upper 256KB of the 0x80000 window. */
    for (uint32_t i = 0; i < MAIN_SIZE; i++)
        shinobi_rom_main[MAIN_SIZE + i] = shinobi_rom_main[i];

    /* sound program + uPD7759 samples (the whole point of this harness) */
    if (!read_exact("epr-11361.a10", shinobi_rom_sound, SOUND_ROM_SIZE)) return 0;
    if (!read_exact("mpr-11362.a11", shinobi_rom_sample, SAMPLE_ROM_SIZE)) return 0;

    s_error[0] = 0;
    return 1;
}
