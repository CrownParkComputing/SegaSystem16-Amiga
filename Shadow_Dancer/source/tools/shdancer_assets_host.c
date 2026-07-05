/* shdancer_assets_host.c -- host (Linux) replacement for the port's
 * hal/shinobi_assets.c. Defines the same extern ROM buffers and the same
 * shinobi_assets_load()/shinobi_assets_error() entry points that
 * shinobi_dyntrans_init() calls, but loads the shdancer ROM set from a normal
 * host directory with stdio and reproduces the EXACT interleave used on target.
 *
 * Layout (identical to hal/shinobi_assets.c):
 *   main   0x80000 : epr-12774b.a6 -> strided off 0, epr-12773b.a5 -> strided off 1
 *   tiles  0x40000 : mpr-12712.b1 -> tp0, mpr-12713.b2 -> tp1, mpr-12714.b3 -> tp2
 *   spr    0x200000: 8 ROMs strided into banks 0/0x80000/0x100000/0x180000,
 *                    a* -> even offset, b* -> odd offset
 *   sound  0x20000 : epr-12987.a4
 *   sample 0x40000 : mpr-12715.b4
 * read_strided(dst,off): src[i] -> dst[off + i*2].
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ROM buffers declared extern in hal/shinobi_assets.h and used by
 * shinobi_interp.c + shinobi_hwrender.c. */
uint8_t *shinobi_rom_main;
uint8_t *shinobi_gfx_tp0;
uint8_t *shinobi_gfx_tp1;
uint8_t *shinobi_gfx_tp2;
uint8_t *shinobi_gfx_spr;
uint8_t *shinobi_rom_sound;
uint8_t *shinobi_rom_sample;

#define MAIN_HALF_SIZE  0x40000u
#define TILE_PLANE_SIZE 0x40000u
#define MAIN_ALLOC_SIZE 0x80000u
#define SPR_REGION_SIZE 0x200000u
#define SOUND_ROM_SIZE  0x20000u
#define SAMPLE_ROM_SIZE 0x40000u

static char s_error[256];
static char s_dir[1024] = "/home/jon/SegaSystem16-Amiga/Shadow_Dancer/roms/shdancer";

void shinobi_assets_host_set_dir(const char *dir)
{
    if (dir && dir[0]) {
        strncpy(s_dir, dir, sizeof s_dir - 1);
        s_dir[sizeof s_dir - 1] = 0;
    }
}

const char *shinobi_assets_error(void)
{
    return s_error[0] ? s_error : "missing Shadow Dancer ROM assets";
}

static int read_exact(const char *fname, uint8_t *dst, uint32_t bytes)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", s_dir, fname);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(s_error, sizeof s_error, "cannot open %s", path);
        return 0;
    }
    size_t got = fread(dst, 1, bytes, f);
    fclose(f);
    if (got != bytes) {
        snprintf(s_error, sizeof s_error, "short read %s (%zu/%u)", path, got, bytes);
        return 0;
    }
    return 1;
}

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

    shinobi_rom_main = (uint8_t *)calloc(1, MAIN_ALLOC_SIZE);
    shinobi_gfx_tp0  = (uint8_t *)calloc(1, TILE_PLANE_SIZE);
    shinobi_gfx_tp1  = (uint8_t *)calloc(1, TILE_PLANE_SIZE);
    shinobi_gfx_tp2  = (uint8_t *)calloc(1, TILE_PLANE_SIZE);
    shinobi_gfx_spr  = (uint8_t *)calloc(1, SPR_REGION_SIZE);
    shinobi_rom_sound  = (uint8_t *)calloc(1, SOUND_ROM_SIZE);
    shinobi_rom_sample = (uint8_t *)calloc(1, SAMPLE_ROM_SIZE);
    if (!shinobi_rom_main || !shinobi_gfx_tp0 || !shinobi_gfx_tp1 ||
        !shinobi_gfx_tp2 || !shinobi_gfx_spr) {
        snprintf(s_error, sizeof s_error, "not enough memory for ROM assets");
        return 0;
    }

    /* maincpu: two 0x40000 halves, 16-bit interleaved (a6=even, a5=odd) -> 0x80000 */
    if (!read_strided("epr-12774b.a6", shinobi_rom_main, 0)) return 0;
    if (!read_strided("epr-12773b.a5", shinobi_rom_main, 1)) return 0;

    /* tiles: 3 planes, sequential, 0x40000 each */
    if (!read_exact("mpr-12712.b1", shinobi_gfx_tp0, TILE_PLANE_SIZE)) return 0;
    if (!read_exact("mpr-12713.b2", shinobi_gfx_tp1, TILE_PLANE_SIZE)) return 0;
    if (!read_exact("mpr-12714.b3", shinobi_gfx_tp2, TILE_PLANE_SIZE)) return 0;

    /* sprites: 4 banks of 0x80000, a*=even byte, b*=odd byte */
    if (!read_strided("mpr-12726.a11", shinobi_gfx_spr, 0x000000)) return 0;
    if (!read_strided("mpr-12719.b11", shinobi_gfx_spr, 0x000001)) return 0;
    if (!read_strided("mpr-12725.a10", shinobi_gfx_spr, 0x080000)) return 0;
    if (!read_strided("mpr-12718.b10", shinobi_gfx_spr, 0x080001)) return 0;
    if (!read_strided("mpr-12724.a9",  shinobi_gfx_spr, 0x100000)) return 0;
    if (!read_strided("mpr-12717.b9",  shinobi_gfx_spr, 0x100001)) return 0;
    if (!read_strided("epr-12723.a8",  shinobi_gfx_spr, 0x180000)) return 0;
    if (!read_strided("epr-12716.b8",  shinobi_gfx_spr, 0x180001)) return 0;

    /* optional sound + samples (not needed for video, loaded for completeness) */
    read_exact("epr-12987.a4", shinobi_rom_sound,  SOUND_ROM_SIZE);
    read_exact("mpr-12715.b4", shinobi_rom_sample, SAMPLE_ROM_SIZE);
    s_error[0] = 0;
    return 1;
}
