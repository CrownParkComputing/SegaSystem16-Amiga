/* shinobi_assets.c -- Alien Syndrome (Sega System 16B, World, UNPROTECTED)
 * runtime ROM loader.  Same shared-HAL structure as the proven Shinobi port;
 * only the ROM set, sizes and interleave differ (aliensyn is plain MC68000, no
 * FD1094/FD1089 key).
 *
 * aliensyn ROM layout (verified against `mame -listxml aliensyn`):
 *   maincpu  192KB = 3 EVEN/ODD pairs of 0x8000 (ROM_LOAD16_BYTE):
 *     @0x00000 epr-11083.a4(even off0) + epr-11080.a1(odd off1)
 *     @0x10000 epr-11084.a5(even)      + epr-11081.a2(odd)
 *     @0x20000 epr-11085.a6(even)      + epr-11082.a3(odd)
 *   tiles    192KB = 3 sequential 0x10000 planes:
 *     epr-10702.b9 -> tp0, epr-10703.b10 -> tp1, epr-10704.b11 -> tp2
 *   sprites  512KB = 4 EVEN/ODD banks of 0x20000 (each ROM 0x10000):
 *     @0x00000 epr-10713.b5 + epr-10709.b1
 *     @0x20000 epr-10714.b6 + epr-10710.b2
 *     @0x40000 epr-10715.b7 + epr-10711.b3
 *     @0x60000 epr-10716.b8 + epr-10712.b4
 *   soundcpu epr-10723.a7 (0x8000 Z80 program) + 3x0x8000 uPD7759 samples
 *     (epr-10724.a8, epr-10725.a9, epr-10726.a10) concatenated.
 */
#include "shinobi_assets.h"

#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dostags.h>
#include <stdint.h>

/* Per-ROM read length for the main program halves (ROM_LOAD16_BYTE, 32KB). */
#define MAIN_HALF_SIZE  0x8000u
/* Per-ROM read length for a single sprite plane ROM (64KB). */
#define SPR_ROM_SIZE    0x10000u
/* Each tile plane ROM (64KB) -> one of tp0/tp1/tp2. */
#define TILE_PLANE_SIZE 0x10000u
/* Assembled main program: 192KB (3 pairs of 0x10000). */
#define MAIN_SIZE       0x30000u
/* The shared System16B interp copies a fixed 0x80000 window to guest $0, so the
 * program buffer must be at least that big. aliensyn's program is only 0x30000;
 * it is loaded at the bottom and the rest is left zero (alloc_clear). No mirror
 * (unlike Shinobi, whose 256KB program is banked into the upper half). */
#define MAIN_ALLOC_SIZE 0x80000u
/* Assembled sprite region: 512KB = 4 banks of 0x20000 (draw code sbase=0x10000*bank). */
#define SPR_REGION_SIZE 0x80000u
/* Z80 sound program. */
#define SOUND_ROM_SIZE  0x8000u
/* uPD7759 samples: 3 * 0x8000 concatenated = 0x18000 of real data. */
#define SAMPLE_ROM_SIZE 0x18000u
/* Allocate the sample buffer at a power of 2 so shinobi_sound.c's 0x1ffff sample
 * address mask always stays in-bounds; the top 0x8000 is zero padding. */
#define SAMPLE_ALLOC_SIZE 0x20000u

uint8_t *shinobi_rom_main;
uint8_t *shinobi_gfx_tp0;
uint8_t *shinobi_gfx_tp1;
uint8_t *shinobi_gfx_tp2;
uint8_t *shinobi_gfx_spr;
uint8_t *shinobi_rom_sound;
uint8_t *shinobi_rom_sample;

static char s_error[192];
struct DosLibrary *DOSBase = 0;

static void scopy(char *dst, const char *src)
{
    while ((*dst++ = *src++)) {}
}

static void sappend(char *dst, const char *src)
{
    while (*dst) dst++;
    while ((*dst++ = *src++)) {}
}

static void set_error(const char *msg)
{
    unsigned i = 0;
    while (msg[i] && i < sizeof(s_error) - 1) {
        s_error[i] = msg[i];
        i++;
    }
    s_error[i] = 0;
}

const char *shinobi_assets_error(void)
{
    return s_error[0] ? s_error : "missing Alien Syndrome ROM assets";
}

static void *alloc_clear(uint32_t bytes)
{
    void *p = AllocMem(bytes, MEMF_FAST | MEMF_CLEAR);
    if (!p) p = AllocMem(bytes, MEMF_PUBLIC | MEMF_CLEAR);
    return p;
}

static int read_exact(const char *path, uint8_t *dst, uint32_t bytes)
{
    if (!DOSBase) DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 36);
    if (!DOSBase) DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 0);
    if (!DOSBase) return 0;
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) return 0;
    uint32_t done = 0;
    while (done < bytes) {
        LONG got = Read(fh, dst + done, bytes - done);
        if (got <= 0) {
            Close(fh);
            return 0;
        }
        done += (uint32_t)got;
    }
    Close(fh);
    return 1;
}

/* Read `len` bytes from `path` and scatter them into dst at byte `offset` with a
 * stride of 2 (ROM_LOAD16_BYTE even/odd interleave). Length-aware so it serves
 * both the 0x8000 main-program halves and the 0x10000 sprite ROMs. */
static int read_strided_n(const char *path, uint8_t *dst, uint32_t offset, uint32_t len)
{
    uint8_t *tmp = (uint8_t*)alloc_clear(len);
    if (!tmp) {
        set_error("not enough memory for ROM import buffer");
        return 0;
    }
    if (!read_exact(path, tmp, len)) {
        FreeMem(tmp, len);
        return 0;
    }
    for (uint32_t i = 0; i < len; i++) dst[offset + i * 2u] = tmp[i];
    FreeMem(tmp, len);
    return 1;
}

static void ensure_rom_dirs(void)
{
    if (!DOSBase) DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 36);
    if (!DOSBase) DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 0);
    if (!DOSBase) return;
    BPTR l;
    l = CreateDir((CONST_STRPTR)"PROGDIR:roms"); if (l) UnLock(l);
    l = CreateDir((CONST_STRPTR)"PROGDIR:roms/aliensyn"); if (l) UnLock(l);
}

static int run_unzip(const char *zip_path)
{
    char cmd[256];
    if (!DOSBase) DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 36);
    if (!DOSBase) DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 0);
    if (!DOSBase) return 0;
    ensure_rom_dirs();
    scopy(cmd, "C:UnZip -oq ");
    sappend(cmd, zip_path);
    sappend(cmd, " -d PROGDIR:roms");
    if (SystemTags((CONST_STRPTR)cmd, SYS_Input, 0, SYS_Output, 0, TAG_DONE) == 0) return 1;

    scopy(cmd, "UnZip -oq ");
    sappend(cmd, zip_path);
    sappend(cmd, " -d PROGDIR:roms");
    return SystemTags((CONST_STRPTR)cmd, SYS_Input, 0, SYS_Output, 0, TAG_DONE) == 0;
}

static void try_import_zip(void)
{
    /* Disabled in the runtime hot path: spawning C:UnZip from this minimal
     * startup path can fault on some Workbench/Amiberry setups. The clean
     * package stays ROM-free; ROMs are injected into roms/aliensyn before launch. */
    (void)run_unzip;
}

static int load_set_from(const char *dir)
{
    char p[96];

    /* maincpu: 3 EVEN/ODD pairs, 0x8000 per ROM, interleaved into 0x00000/0x10000/0x20000 */
    scopy(p, dir); sappend(p, "/epr-11083.a4");
    if (!read_strided_n(p, shinobi_rom_main, 0x00000, MAIN_HALF_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-11080.a1");
    if (!read_strided_n(p, shinobi_rom_main, 0x00001, MAIN_HALF_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-11084.a5");
    if (!read_strided_n(p, shinobi_rom_main, 0x10000, MAIN_HALF_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-11081.a2");
    if (!read_strided_n(p, shinobi_rom_main, 0x10001, MAIN_HALF_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-11085.a6");
    if (!read_strided_n(p, shinobi_rom_main, 0x20000, MAIN_HALF_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-11082.a3");
    if (!read_strided_n(p, shinobi_rom_main, 0x20001, MAIN_HALF_SIZE)) return 0;

    /* tiles: 3 sequential 0x10000 planes */
    scopy(p, dir); sappend(p, "/epr-10702.b9");
    if (!read_exact(p, shinobi_gfx_tp0, TILE_PLANE_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-10703.b10");
    if (!read_exact(p, shinobi_gfx_tp1, TILE_PLANE_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-10704.b11");
    if (!read_exact(p, shinobi_gfx_tp2, TILE_PLANE_SIZE)) return 0;

    /* sprites: 4 EVEN/ODD banks of 0x20000; 0x10000 per ROM, strided 2 */
    scopy(p, dir); sappend(p, "/epr-10713.b5");
    if (!read_strided_n(p, shinobi_gfx_spr, 0x00000, SPR_ROM_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-10709.b1");
    if (!read_strided_n(p, shinobi_gfx_spr, 0x00001, SPR_ROM_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-10714.b6");
    if (!read_strided_n(p, shinobi_gfx_spr, 0x20000, SPR_ROM_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-10710.b2");
    if (!read_strided_n(p, shinobi_gfx_spr, 0x20001, SPR_ROM_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-10715.b7");
    if (!read_strided_n(p, shinobi_gfx_spr, 0x40000, SPR_ROM_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-10711.b3");
    if (!read_strided_n(p, shinobi_gfx_spr, 0x40001, SPR_ROM_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-10716.b8");
    if (!read_strided_n(p, shinobi_gfx_spr, 0x60000, SPR_ROM_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-10712.b4");
    if (!read_strided_n(p, shinobi_gfx_spr, 0x60001, SPR_ROM_SIZE)) return 0;

    /* No mirror: aliensyn's 0x30000 program sits at the bottom; the rest of the
     * 0x80000 window stays zero (matches MAME's zero-padded 0x40000 region). */
    return 1;
}

static int try_load_sound_from(const char *dir)
{
    char p[96];
    int ok = 0;

    if (!shinobi_rom_sound)
        shinobi_rom_sound = (uint8_t*)alloc_clear(SOUND_ROM_SIZE);
    if (!shinobi_rom_sample)
        shinobi_rom_sample = (uint8_t*)alloc_clear(SAMPLE_ALLOC_SIZE);
    if (!shinobi_rom_sound || !shinobi_rom_sample)
        return 0;

    scopy(p, dir); sappend(p, "/epr-10723.a7");
    if (read_exact(p, shinobi_rom_sound, SOUND_ROM_SIZE))
        ok = 1;

    /* Three 0x8000 sample ROMs concatenated into a single 0x18000 region. */
    scopy(p, dir); sappend(p, "/epr-10724.a8");
    if (read_exact(p, shinobi_rom_sample + 0x0000u, 0x8000u))
        ok = 1;
    scopy(p, dir); sappend(p, "/epr-10725.a9");
    if (read_exact(p, shinobi_rom_sample + 0x8000u, 0x8000u))
        ok = 1;
    scopy(p, dir); sappend(p, "/epr-10726.a10");
    if (read_exact(p, shinobi_rom_sample + 0x10000u, 0x8000u))
        ok = 1;

    (void)SAMPLE_ROM_SIZE;
    return ok;
}

static int load_optional_sound_for(const char *dir)
{
    if (try_load_sound_from(dir))
        return 1;
    if (try_load_sound_from("PROGDIR:roms/aliensyn"))
        return 1;
    return try_load_sound_from("PROGDIR:roms");
}

int shinobi_assets_load(void)
{
    if (shinobi_rom_main && shinobi_gfx_tp0 && shinobi_gfx_tp1 && shinobi_gfx_tp2 && shinobi_gfx_spr)
        return 1;

    shinobi_rom_main = (uint8_t*)alloc_clear(MAIN_ALLOC_SIZE);
    shinobi_gfx_tp0 = (uint8_t*)alloc_clear(TILE_PLANE_SIZE);
    shinobi_gfx_tp1 = (uint8_t*)alloc_clear(TILE_PLANE_SIZE);
    shinobi_gfx_tp2 = (uint8_t*)alloc_clear(TILE_PLANE_SIZE);
    shinobi_gfx_spr = (uint8_t*)alloc_clear(SPR_REGION_SIZE);
    if (!shinobi_rom_main || !shinobi_gfx_tp0 || !shinobi_gfx_tp1 || !shinobi_gfx_tp2 || !shinobi_gfx_spr) {
        set_error("not enough memory for Alien Syndrome ROM assets");
        return 0;
    }

    if (load_set_from("PROGDIR:roms/aliensyn")) { load_optional_sound_for("PROGDIR:roms/aliensyn"); return 1; }
    if (load_set_from("PROGDIR:roms")) { load_optional_sound_for("PROGDIR:roms"); return 1; }

    try_import_zip();

    if (load_set_from("PROGDIR:roms/aliensyn")) { load_optional_sound_for("PROGDIR:roms/aliensyn"); return 1; }
    if (load_set_from("PROGDIR:roms")) { load_optional_sound_for("PROGDIR:roms"); return 1; }

    set_error("extract aliensyn.zip into roms/ so roms/aliensyn contains the ROM files");
    return 0;
}
