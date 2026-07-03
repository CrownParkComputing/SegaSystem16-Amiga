#include "shinobi_assets.h"

#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dostags.h>
#include <stdint.h>

#define MAIN_HALF_SIZE 0x20000u
#define TILE_PLANE_SIZE 0x20000u
#define SPR_ROM_SIZE 0x40000u
#define MAIN_SIZE 0x80000u
#define SPR_REGION_SIZE 0x1c0000u
#define SOUND_ROM_SIZE 0x8000u
#define SAMPLE_ROM_SIZE 0x20000u

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
    return s_error[0] ? s_error : "missing Golden Axe ROM assets";
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

static int read_strided(const char *path, uint8_t *dst, uint32_t offset)
{
    uint8_t *tmp = (uint8_t*)alloc_clear(MAIN_HALF_SIZE);
    if (!tmp) {
        set_error("not enough memory for ROM import buffer");
        return 0;
    }
    if (!read_exact(path, tmp, MAIN_HALF_SIZE)) {
        FreeMem(tmp, MAIN_HALF_SIZE);
        return 0;
    }
    for (uint32_t i = 0; i < MAIN_HALF_SIZE; i++) dst[offset + i * 2u] = tmp[i];
    FreeMem(tmp, MAIN_HALF_SIZE);
    return 1;
}

static int read_sprite_continued(const char *path, uint8_t *dst, uint32_t offset0, uint32_t offset1)
{
    uint8_t *tmp = (uint8_t*)alloc_clear(SPR_ROM_SIZE);
    if (!tmp) {
        set_error("not enough memory for sprite ROM import buffer");
        return 0;
    }
    if (!read_exact(path, tmp, SPR_ROM_SIZE)) {
        FreeMem(tmp, SPR_ROM_SIZE);
        return 0;
    }
    for (uint32_t i = 0; i < MAIN_HALF_SIZE; i++) {
        dst[offset0 + i * 2u] = tmp[i];
        dst[offset1 + i * 2u] = tmp[MAIN_HALF_SIZE + i];
    }
    FreeMem(tmp, SPR_ROM_SIZE);
    return 1;
}

static void ensure_rom_dirs(void)
{
    if (!DOSBase) DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 36);
    if (!DOSBase) DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 0);
    if (!DOSBase) return;
    BPTR l;
    l = CreateDir((CONST_STRPTR)"PROGDIR:roms"); if (l) UnLock(l);
    l = CreateDir((CONST_STRPTR)"PROGDIR:roms/goldnaxe3d"); if (l) UnLock(l);
    l = CreateDir((CONST_STRPTR)"PROGDIR:roms/goldnaxe2"); if (l) UnLock(l);
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
    /* Disabled in the runtime hot path for now. Spawning C:UnZip from this
     * minimal startup path can fault on some Workbench/Amiberry setups. The
     * clean package remains ROM-free; install/import extracts the user zip into
     * roms/ on the host/shared folder before launch. */
}

static int load_set_from(const char *dir)
{
    char p[96];

    scopy(p, dir); sappend(p, "/goldnaxe3d/bootleg_epr-12525.a7");
    if (!read_strided(p, shinobi_rom_main, 0)) return 0;
    scopy(p, dir); sappend(p, "/goldnaxe3d/bootleg_epr-12524.a5");
    if (!read_strided(p, shinobi_rom_main, 1)) return 0;
    scopy(p, dir); sappend(p, "/goldnaxe2/epr-12521.a8");
    if (!read_strided(p, shinobi_rom_main, 0x40000)) return 0;
    scopy(p, dir); sappend(p, "/goldnaxe2/epr-12519.a6");
    if (!read_strided(p, shinobi_rom_main, 0x40001)) return 0;

    scopy(p, dir); sappend(p, "/epr-12385.ic19");
    if (!read_exact(p, shinobi_gfx_tp0, TILE_PLANE_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-12386.ic20");
    if (!read_exact(p, shinobi_gfx_tp1, TILE_PLANE_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/epr-12387.ic21");
    if (!read_exact(p, shinobi_gfx_tp2, TILE_PLANE_SIZE)) return 0;

    scopy(p, dir); sappend(p, "/mpr-12378.ic9");
    if (!read_sprite_continued(p, shinobi_gfx_spr, 0x000001, 0x100001)) return 0;
    scopy(p, dir); sappend(p, "/mpr-12379.ic12");
    if (!read_sprite_continued(p, shinobi_gfx_spr, 0x000000, 0x100000)) return 0;
    scopy(p, dir); sappend(p, "/mpr-12380.ic10");
    if (!read_sprite_continued(p, shinobi_gfx_spr, 0x040001, 0x140001)) return 0;
    scopy(p, dir); sappend(p, "/mpr-12381.ic13");
    if (!read_sprite_continued(p, shinobi_gfx_spr, 0x040000, 0x140000)) return 0;
    scopy(p, dir); sappend(p, "/mpr-12382.ic11");
    if (!read_sprite_continued(p, shinobi_gfx_spr, 0x080001, 0x180001)) return 0;
    scopy(p, dir); sappend(p, "/mpr-12383.ic14");
    if (!read_sprite_continued(p, shinobi_gfx_spr, 0x080000, 0x180000)) return 0;

    return 1;
}

static int try_load_sound_from(const char *dir)
{
    char p[96];
    int ok = 0;

    if (!shinobi_rom_sound)
        shinobi_rom_sound = (uint8_t*)alloc_clear(SOUND_ROM_SIZE);
    if (!shinobi_rom_sample)
        shinobi_rom_sample = (uint8_t*)alloc_clear(SAMPLE_ROM_SIZE);
    if (!shinobi_rom_sound || !shinobi_rom_sample)
        return 0;

    scopy(p, dir); sappend(p, "/epr-12390.ic8");
    if (read_exact(p, shinobi_rom_sound, SOUND_ROM_SIZE))
        ok = 1;

    scopy(p, dir); sappend(p, "/mpr-12384.ic6");
    if (read_exact(p, shinobi_rom_sample, SAMPLE_ROM_SIZE))
        ok = 1;

    return ok;
}

static int load_optional_sound_for(const char *dir)
{
    return try_load_sound_from("PROGDIR:roms");
}

int shinobi_assets_load(void)
{
    if (shinobi_rom_main && shinobi_gfx_tp0 && shinobi_gfx_tp1 && shinobi_gfx_tp2 && shinobi_gfx_spr)
        return 1;

    shinobi_rom_main = (uint8_t*)alloc_clear(MAIN_SIZE);
    shinobi_gfx_tp0 = (uint8_t*)alloc_clear(TILE_PLANE_SIZE);
    shinobi_gfx_tp1 = (uint8_t*)alloc_clear(TILE_PLANE_SIZE);
    shinobi_gfx_tp2 = (uint8_t*)alloc_clear(TILE_PLANE_SIZE);
    shinobi_gfx_spr = (uint8_t*)alloc_clear(SPR_REGION_SIZE);
    if (!shinobi_rom_main || !shinobi_gfx_tp0 || !shinobi_gfx_tp1 || !shinobi_gfx_tp2 || !shinobi_gfx_spr) {
        set_error("not enough memory for Golden Axe ROM assets");
        return 0;
    }

    if (load_set_from("PROGDIR:roms")) { load_optional_sound_for("PROGDIR:roms"); return 1; }

    try_import_zip();

    if (load_set_from("PROGDIR:roms")) { load_optional_sound_for("PROGDIR:roms"); return 1; }

    set_error("extract goldnaxe.zip into roms/ so roms/goldnaxe3d and common ROMs are present");
    return 0;
}
