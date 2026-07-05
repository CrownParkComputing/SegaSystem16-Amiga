#include "shinobi_assets.h"

#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dostags.h>
#include <stdint.h>

/* Shadow Dancer (shdancer, World, unprotected) -- Sega System 16B-compatible.
 * Each main/sprite/tile ROM is 0x40000 bytes. Main program is a full 0x80000
 * (two 0x40000 halves interleaved) -- the shared interp copies 0x80000 to guest
 * $0, so the buffer is exactly 0x80000 with NO mirror (unlike 256KB Shinobi).
 * Sprites are 4 interleaved 0x80000 banks = 0x200000. Sound Z80 program is
 * 128KB (0x20000); uPD/RF PCM samples are 256KB (0x40000). */
#define MAIN_HALF_SIZE 0x40000u
#define TILE_PLANE_SIZE 0x40000u
#define SPR_ROM_SIZE 0x40000u
#define MAIN_SIZE 0x80000u
#define MAIN_ALLOC_SIZE 0x80000u
#define SPR_REGION_SIZE 0x200000u
#define SOUND_ROM_SIZE 0x20000u
#define SAMPLE_ROM_SIZE 0x40000u

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
    return s_error[0] ? s_error : "missing Shadow Dancer ROM assets";
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

static void ensure_rom_dirs(void)
{
    if (!DOSBase) DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 36);
    if (!DOSBase) DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 0);
    if (!DOSBase) return;
    BPTR l;
    l = CreateDir((CONST_STRPTR)"PROGDIR:roms"); if (l) UnLock(l);
    l = CreateDir((CONST_STRPTR)"PROGDIR:roms/shdancer"); if (l) UnLock(l);
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
     * roms/shdancer on the host/shared folder before launch. */
}

static int load_set_from(const char *dir)
{
    char p[96];

    /* maincpu: two 0x40000 halves, 16-bit interleaved (a6=even, a5=odd) -> 0x80000 */
    scopy(p, dir); sappend(p, "/epr-12774b.a6");
    if (!read_strided(p, shinobi_rom_main, 0)) return 0;
    scopy(p, dir); sappend(p, "/epr-12773b.a5");
    if (!read_strided(p, shinobi_rom_main, 1)) return 0;

    /* tiles: 3 planes, sequential, 0x40000 each */
    scopy(p, dir); sappend(p, "/mpr-12712.b1");
    if (!read_exact(p, shinobi_gfx_tp0, TILE_PLANE_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/mpr-12713.b2");
    if (!read_exact(p, shinobi_gfx_tp1, TILE_PLANE_SIZE)) return 0;
    scopy(p, dir); sappend(p, "/mpr-12714.b3");
    if (!read_exact(p, shinobi_gfx_tp2, TILE_PLANE_SIZE)) return 0;

    /* sprites: 4 banks of 0x80000, each bank = even(a*)+odd(b*) 16-bit interleave.
     * a* -> even byte at bank base, b* -> odd byte. Each ROM is 0x40000. */
    scopy(p, dir); sappend(p, "/mpr-12726.a11");
    if (!read_strided(p, shinobi_gfx_spr, 0x000000)) return 0;   /* bank0 even */
    scopy(p, dir); sappend(p, "/mpr-12719.b11");
    if (!read_strided(p, shinobi_gfx_spr, 0x000001)) return 0;   /* bank0 odd  */
    scopy(p, dir); sappend(p, "/mpr-12725.a10");
    if (!read_strided(p, shinobi_gfx_spr, 0x080000)) return 0;   /* bank1 even */
    scopy(p, dir); sappend(p, "/mpr-12718.b10");
    if (!read_strided(p, shinobi_gfx_spr, 0x080001)) return 0;   /* bank1 odd  */
    scopy(p, dir); sappend(p, "/mpr-12724.a9");
    if (!read_strided(p, shinobi_gfx_spr, 0x100000)) return 0;   /* bank2 even */
    scopy(p, dir); sappend(p, "/mpr-12717.b9");
    if (!read_strided(p, shinobi_gfx_spr, 0x100001)) return 0;   /* bank2 odd  */
    scopy(p, dir); sappend(p, "/epr-12723.a8");
    if (!read_strided(p, shinobi_gfx_spr, 0x180000)) return 0;   /* bank3 even */
    scopy(p, dir); sappend(p, "/epr-12716.b8");
    if (!read_strided(p, shinobi_gfx_spr, 0x180001)) return 0;   /* bank3 odd  */

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

    scopy(p, dir); sappend(p, "/epr-12987.a4");
    if (read_exact(p, shinobi_rom_sound, SOUND_ROM_SIZE))
        ok = 1;

    scopy(p, dir); sappend(p, "/mpr-12715.b4");
    if (read_exact(p, shinobi_rom_sample, SAMPLE_ROM_SIZE))
        ok = 1;

    return ok;
}

static int load_optional_sound_for(const char *dir)
{
    if (try_load_sound_from(dir))
        return 1;
    if (try_load_sound_from("PROGDIR:roms/shdancer"))
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
        set_error("not enough memory for Shadow Dancer ROM assets");
        return 0;
    }

    if (load_set_from("PROGDIR:roms/shdancer")) { load_optional_sound_for("PROGDIR:roms/shdancer"); return 1; }
    if (load_set_from("PROGDIR:roms")) { load_optional_sound_for("PROGDIR:roms"); return 1; }

    try_import_zip();

    if (load_set_from("PROGDIR:roms/shdancer")) { load_optional_sound_for("PROGDIR:roms/shdancer"); return 1; }
    if (load_set_from("PROGDIR:roms")) { load_optional_sound_for("PROGDIR:roms"); return 1; }

    set_error("extract shdancer.zip into roms/ so roms/shdancer contains the ROM files");
    return 0;
}
