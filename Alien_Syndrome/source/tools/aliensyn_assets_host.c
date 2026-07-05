/* aliensyn_assets_host.c -- host (Linux) replacement for the port's
 * hal/shinobi_assets.c. Defines the same extern ROM buffers and the same
 * shinobi_assets_load()/shinobi_assets_error() entry points that
 * shinobi_dyntrans_init() calls, but loads the aliensyn (Sega System 16B,
 * UNPROTECTED plain 68000) ROM set from a normal host directory with stdio and
 * reproduces the EXACT interleave used on target (byte-for-byte identical to
 * hal/shinobi_assets.c, itself verified against `mame -listxml aliensyn`).
 *
 * Layout (identical to hal/shinobi_assets.c):
 *   main   0x80000 alloc, 0x30000 program at bottom (rest zero-pad):
 *          3 EVEN/ODD pairs, each ROM 0x8000 (ROM_LOAD16_BYTE, stride 2):
 *            @0x00000 epr-11083.a4(even off0) + epr-11080.a1(odd off1)
 *            @0x10000 epr-11084.a5(even)      + epr-11081.a2(odd)
 *            @0x20000 epr-11085.a6(even)      + epr-11082.a3(odd)
 *   tiles  3 planes 0x10000 each, sequential:
 *            epr-10702.b9 -> tp0, epr-10703.b10 -> tp1, epr-10704.b11 -> tp2
 *   spr    0x80000 region, 4 EVEN/ODD banks of 0x20000 (each ROM 0x10000, stride 2):
 *            @0x00000 epr-10713.b5(even) + epr-10709.b1(odd)
 *            @0x20000 epr-10714.b6       + epr-10710.b2
 *            @0x40000 epr-10715.b7       + epr-10711.b3
 *            @0x60000 epr-10716.b8       + epr-10712.b4
 *   sound  0x8000 : epr-10723.a7 -> shinobi_rom_sound
 *   sample 0x20000 alloc, 0x18000 data: epr-10724.a8 + epr-10725.a9 + epr-10726.a10
 *          (each 0x8000) concatenated -> shinobi_rom_sample
 * read_strided_n(dst,off,len): src[i] -> dst[off + i*2], i<len.
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

#define MAIN_HALF_SIZE    0x8000u   /* per main-program ROM (ROM_LOAD16_BYTE) */
#define SPR_ROM_SIZE      0x10000u  /* per sprite plane ROM */
#define TILE_PLANE_SIZE   0x10000u  /* per tile plane ROM -> tp0/tp1/tp2 */
#define MAIN_ALLOC_SIZE   0x80000u  /* interp copies a fixed 0x80000 window to $0 */
#define SPR_REGION_SIZE   0x80000u  /* 4 banks of 0x20000 */
#define SOUND_ROM_SIZE    0x8000u
#define SAMPLE_ALLOC_SIZE 0x20000u  /* 0x18000 data + 0x8000 zero pad (mask safety) */

static char s_error[256];
static char s_dir[1024] = "/home/jon/SegaSystem16-Amiga/Alien_Syndrome/roms/aliensyn";

void shinobi_assets_host_set_dir(const char *dir)
{
    if (dir && dir[0]) {
        strncpy(s_dir, dir, sizeof s_dir - 1);
        s_dir[sizeof s_dir - 1] = 0;
    }
}

const char *shinobi_assets_error(void)
{
    return s_error[0] ? s_error : "missing Alien Syndrome ROM assets";
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

/* Read `len` bytes from `fname` and scatter into dst at byte `offset` with a
 * stride of 2 (ROM_LOAD16_BYTE even/odd interleave). Length-aware so it serves
 * both the 0x8000 main-program halves and the 0x10000 sprite ROMs. */
static int read_strided_n(const char *fname, uint8_t *dst, uint32_t offset, uint32_t len)
{
    uint8_t *tmp = (uint8_t *)malloc(len);
    if (!tmp) { snprintf(s_error, sizeof s_error, "oom import buffer"); return 0; }
    if (!read_exact(fname, tmp, len)) { free(tmp); return 0; }
    for (uint32_t i = 0; i < len; i++)
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
    shinobi_rom_sample = (uint8_t *)calloc(1, SAMPLE_ALLOC_SIZE);
    if (!shinobi_rom_main || !shinobi_gfx_tp0 || !shinobi_gfx_tp1 ||
        !shinobi_gfx_tp2 || !shinobi_gfx_spr) {
        snprintf(s_error, sizeof s_error, "not enough memory for ROM assets");
        return 0;
    }

    /* maincpu: 3 EVEN/ODD pairs, 0x8000 per ROM, interleaved into 0x00000/0x10000/0x20000.
     * Program is 0x30000; rest of the 0x80000 window stays zero (calloc). */
    if (!read_strided_n("epr-11083.a4", shinobi_rom_main, 0x00000, MAIN_HALF_SIZE)) return 0;
    if (!read_strided_n("epr-11080.a1", shinobi_rom_main, 0x00001, MAIN_HALF_SIZE)) return 0;
    if (!read_strided_n("epr-11084.a5", shinobi_rom_main, 0x10000, MAIN_HALF_SIZE)) return 0;
    if (!read_strided_n("epr-11081.a2", shinobi_rom_main, 0x10001, MAIN_HALF_SIZE)) return 0;
    if (!read_strided_n("epr-11085.a6", shinobi_rom_main, 0x20000, MAIN_HALF_SIZE)) return 0;
    if (!read_strided_n("epr-11082.a3", shinobi_rom_main, 0x20001, MAIN_HALF_SIZE)) return 0;

    /* tiles: 3 planes, sequential, 0x10000 each */
    if (!read_exact("epr-10702.b9",  shinobi_gfx_tp0, TILE_PLANE_SIZE)) return 0;
    if (!read_exact("epr-10703.b10", shinobi_gfx_tp1, TILE_PLANE_SIZE)) return 0;
    if (!read_exact("epr-10704.b11", shinobi_gfx_tp2, TILE_PLANE_SIZE)) return 0;

    /* sprites: 4 EVEN/ODD banks of 0x20000; 0x10000 per ROM, strided 2 */
    if (!read_strided_n("epr-10713.b5", shinobi_gfx_spr, 0x00000, SPR_ROM_SIZE)) return 0;
    if (!read_strided_n("epr-10709.b1", shinobi_gfx_spr, 0x00001, SPR_ROM_SIZE)) return 0;
    if (!read_strided_n("epr-10714.b6", shinobi_gfx_spr, 0x20000, SPR_ROM_SIZE)) return 0;
    if (!read_strided_n("epr-10710.b2", shinobi_gfx_spr, 0x20001, SPR_ROM_SIZE)) return 0;
    if (!read_strided_n("epr-10715.b7", shinobi_gfx_spr, 0x40000, SPR_ROM_SIZE)) return 0;
    if (!read_strided_n("epr-10711.b3", shinobi_gfx_spr, 0x40001, SPR_ROM_SIZE)) return 0;
    if (!read_strided_n("epr-10716.b8", shinobi_gfx_spr, 0x60000, SPR_ROM_SIZE)) return 0;
    if (!read_strided_n("epr-10712.b4", shinobi_gfx_spr, 0x60001, SPR_ROM_SIZE)) return 0;

    /* optional sound + samples (not needed for video, loaded for completeness) */
    read_exact("epr-10723.a7",  shinobi_rom_sound,           SOUND_ROM_SIZE);
    read_exact("epr-10724.a8",  shinobi_rom_sample + 0x0000, 0x8000u);
    read_exact("epr-10725.a9",  shinobi_rom_sample + 0x8000, 0x8000u);
    read_exact("epr-10726.a10", shinobi_rom_sample + 0x10000, 0x8000u);

    s_error[0] = 0;
    return 1;
}
