/* shinobi_interp.c -- PURE MUSASHI INTERPRETER backend for the Shinobi RTG build.
 *
 * Drop-in replacement for the dyntrans JIT: provides the same entry points
 * (shinobi_dyntrans_init/frame/set_inputs) but runs the real 68000 program
 * through the Musashi *interpreter* over a flat 16MB guest buffer. No native
 * code emission, no relocation, no supervisor -- everything is emulated in C,
 * so it runs in USER mode and coexists with the OS + RTG. Slow but correct:
 * a TEST that the render/RTG/game path works end-to-end on the Amiga.
 *
 * Memory model + I/O mirror the proven host reference tools/shinobi_host.c.
 * Render via shinobi_render(gbase) (shinobi_hwrender.c, -DSHINOBI_RTG).
 */
#include <exec/exec.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <stdint.h>
#include <string.h>
#include "m68k.h"
#include "shinobi_assets.h"

#define GUEST_SIZE 0x1000000u                /* full 24-bit guest space, flat */
extern void shinobi_render(uint8_t *gbase);
extern void shinobi_set_screen_flip(int flip);
extern void shinobi_set_tile_bank(int which, int bank);
extern void shinobi_mark_palette_dirty(void);
extern void shinobi_mark_tile_dirty(unsigned addr);
extern void shinobi_audio_command(uint8_t v);
extern void shinobi_audio_pulse(uint8_t v);
extern uint8_t shinobi_audio_response(void);

static uint8_t *gbase;
static uint8_t in_service = 0xff, in_p1 = 0xff, in_p2 = 0xff;
static uint8_t dsw1 = 0xff, dsw2 = 0xff;
static uint8_t misc_output = 0x00;
static uint8_t mapper_regs[0x20];
static uint8_t tile_bank_regs[2] = { 0, 1 };
static unsigned frame_counter;

extern struct DosLibrary *DOSBase;

static unsigned read_bytes(unsigned a, int size);
static void write_bytes(unsigned a, unsigned v, int size);

static void set_tile_bank_latch(int which, int bank)
{
    if ((unsigned)which >= 2)
        return;
    bank &= 7;
    tile_bank_regs[which] = (uint8_t)bank;
    shinobi_set_tile_bank(which, bank);
}

#define GA_STATE_MAGIC 0x47415853u /* GAXS */
#define GA_STATE_VERSION 2u
#define GA_STATE_PATH "PROGDIR:GoldenAxe.state"

struct ga_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t guest_size;
    uint32_t reg_count;
    uint32_t frame_counter;
    uint8_t mapper_regs[0x20];
    uint8_t tile_bank_regs[2];
    uint8_t input_state[6];
};

static const m68k_register_t state_regs[] = {
    M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3,
    M68K_REG_D4, M68K_REG_D5, M68K_REG_D6, M68K_REG_D7,
    M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3,
    M68K_REG_A4, M68K_REG_A5, M68K_REG_A6, M68K_REG_A7,
    M68K_REG_PC, M68K_REG_SR, M68K_REG_USP, M68K_REG_ISP,
    M68K_REG_MSP, M68K_REG_VBR, M68K_REG_SFC, M68K_REG_DFC,
    M68K_REG_CACR, M68K_REG_CAAR, M68K_REG_PPC, M68K_REG_IR
};

static int ensure_dos(void)
{
    if (!DOSBase)
        DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 36);
    if (!DOSBase)
        DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 0);
    return DOSBase != 0;
}

static int write_exact_bptr(BPTR fh, const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t*)buf;
    while (len) {
        LONG n = Write(fh, (APTR)p, len);
        if (n <= 0)
            return 0;
        p += (uint32_t)n;
        len -= (uint32_t)n;
    }
    return 1;
}

static int read_exact_bptr(BPTR fh, void *buf, uint32_t len)
{
    uint8_t *p = (uint8_t*)buf;
    while (len) {
        LONG n = Read(fh, p, len);
        if (n <= 0)
            return 0;
        p += (uint32_t)n;
        len -= (uint32_t)n;
    }
    return 1;
}

static int state_write_ram(BPTR fh)
{
    return write_exact_bptr(fh, gbase + 0x200000u, 0x40000u) &&
           write_exact_bptr(fh, gbase + 0x400000u, 0x10000u) &&
           write_exact_bptr(fh, gbase + 0x410000u, 0x01000u) &&
           write_exact_bptr(fh, gbase + 0x440000u, 0x00800u) &&
           write_exact_bptr(fh, gbase + 0x840000u, 0x01000u) &&
           write_exact_bptr(fh, gbase + 0xffc000u, 0x04000u);
}

static int state_read_ram(BPTR fh)
{
    return read_exact_bptr(fh, gbase + 0x200000u, 0x40000u) &&
           read_exact_bptr(fh, gbase + 0x400000u, 0x10000u) &&
           read_exact_bptr(fh, gbase + 0x410000u, 0x01000u) &&
           read_exact_bptr(fh, gbase + 0x440000u, 0x00800u) &&
           read_exact_bptr(fh, gbase + 0x840000u, 0x01000u) &&
           read_exact_bptr(fh, gbase + 0xffc000u, 0x04000u);
}

int shinobi_state_exists(void)
{
    struct ga_state_header hdr;
    BPTR fh;
    int ok;
    if (!ensure_dos())
        return 0;
    fh = Open((CONST_STRPTR)GA_STATE_PATH, MODE_OLDFILE);
    if (!fh)
        return 0;
    ok = read_exact_bptr(fh, &hdr, sizeof hdr) &&
         hdr.magic == GA_STATE_MAGIC &&
         hdr.version == GA_STATE_VERSION &&
         hdr.guest_size == GUEST_SIZE &&
         hdr.reg_count == (uint32_t)(sizeof state_regs / sizeof state_regs[0]);
    Close(fh);
    return ok;
}

int shinobi_state_save(void)
{
    struct ga_state_header hdr;
    uint32_t regs[sizeof state_regs / sizeof state_regs[0]];
    BPTR fh;

    if (!gbase || !ensure_dos())
        return 0;

    memset(&hdr, 0, sizeof hdr);
    hdr.magic = GA_STATE_MAGIC;
    hdr.version = GA_STATE_VERSION;
    hdr.guest_size = GUEST_SIZE;
    hdr.reg_count = (uint32_t)(sizeof state_regs / sizeof state_regs[0]);
    hdr.frame_counter = frame_counter;
    memcpy(hdr.mapper_regs, mapper_regs, sizeof mapper_regs);
    memcpy(hdr.tile_bank_regs, tile_bank_regs, sizeof tile_bank_regs);
    hdr.input_state[0] = in_service;
    hdr.input_state[1] = in_p1;
    hdr.input_state[2] = in_p2;
    hdr.input_state[3] = dsw1;
    hdr.input_state[4] = dsw2;
    hdr.input_state[5] = misc_output;

    for (unsigned i = 0; i < hdr.reg_count; i++)
        regs[i] = m68k_get_reg(0, state_regs[i]);

    fh = Open((CONST_STRPTR)GA_STATE_PATH, MODE_NEWFILE);
    if (!fh)
        return 0;
    int ok = write_exact_bptr(fh, &hdr, sizeof hdr) &&
             write_exact_bptr(fh, regs, sizeof regs) &&
             state_write_ram(fh);
    Close(fh);
    return ok;
}

int shinobi_state_load(void)
{
    struct ga_state_header hdr;
    uint32_t regs[sizeof state_regs / sizeof state_regs[0]];
    BPTR fh;

    if (!gbase || !ensure_dos())
        return 0;

    fh = Open((CONST_STRPTR)GA_STATE_PATH, MODE_OLDFILE);
    if (!fh)
        return 0;

    int ok = read_exact_bptr(fh, &hdr, sizeof hdr);
    ok = ok && hdr.magic == GA_STATE_MAGIC &&
         hdr.version == GA_STATE_VERSION &&
         hdr.guest_size == GUEST_SIZE &&
         hdr.reg_count == (uint32_t)(sizeof state_regs / sizeof state_regs[0]);
    ok = ok && read_exact_bptr(fh, regs, sizeof regs);
    ok = ok && state_read_ram(fh);
    Close(fh);
    if (!ok)
        return 0;

    memcpy(mapper_regs, hdr.mapper_regs, sizeof mapper_regs);
    memcpy(tile_bank_regs, hdr.tile_bank_regs, sizeof tile_bank_regs);
    in_service = hdr.input_state[0];
    in_p1 = hdr.input_state[1];
    in_p2 = hdr.input_state[2];
    dsw1 = hdr.input_state[3];
    dsw2 = hdr.input_state[4];
    misc_output = hdr.input_state[5];
    frame_counter = hdr.frame_counter;

    for (unsigned i = 0; i < 16; i++)
        m68k_set_reg(state_regs[i], regs[i]);
    m68k_set_reg(M68K_REG_SR, regs[17]);
    m68k_set_reg(M68K_REG_USP, regs[18]);
    m68k_set_reg(M68K_REG_ISP, regs[19]);
    m68k_set_reg(M68K_REG_MSP, regs[20]);
    for (unsigned i = 21; i < (sizeof state_regs / sizeof state_regs[0]); i++)
        m68k_set_reg(state_regs[i], regs[i]);
    m68k_set_reg(M68K_REG_PC, regs[16]);
    m68k_set_irq(0);

    shinobi_mark_palette_dirty();
    shinobi_set_tile_bank(0, tile_bank_regs[0]);
    shinobi_set_tile_bank(1, tile_bank_regs[1]);
    shinobi_set_screen_flip((misc_output & 0x40) != 0);
    return 1;
}

enum mapped_kind {
    MAP_NONE = 0,
    MAP_ROM,
    MAP_RAM,
    MAP_IO,
    MAP_TILEBANK
};

struct map_hit {
    enum mapped_kind kind;
    unsigned backing;
    unsigned offset;
};

static int int_ack(int level)
{
    (void)level;
    m68k_set_irq(0);
    return M68K_INT_ACK_AUTOVECTOR;
}

static unsigned io_read_offset(unsigned off_bytes, int size)
{
    unsigned word_off = (off_bytes >> 1) & 0x1fff;
    unsigned val = 0xffff;
    switch (word_off & (0x3000 / 2)) {
    case 0x1000 / 2:
        switch (word_off & 3) {
        case 0: val = in_service; break;
        case 1: val = in_p1; break;
        case 2: val = 0xff; break;
        case 3: val = in_p2; break;
        }
        break;
    case 0x2000 / 2:
        val = (word_off & 1) ? dsw1 : dsw2;
        break;
    default:
        val = 0xffff;
        break;
    }
    return size == 1 ? (val & 0xff) : (0xff00 | (val & 0xff));
}

static int compute_region(unsigned index, unsigned length, unsigned mirror, unsigned offset,
                          unsigned a, unsigned *hit_off)
{
    static const unsigned size_mask_map[4] = { 0x00ffffu, 0x01ffffu, 0x07ffffu, 0x1fffffu };
    unsigned size_mask = size_mask_map[mapper_regs[0x10 + 2 * index] & 3u];
    unsigned base = ((unsigned)mapper_regs[0x11 + 2 * index] << 16) & ~size_mask;
    unsigned mir = mirror & size_mask;
    unsigned start = base + (offset & size_mask);
    unsigned span = length - 1u;
    if (span > size_mask)
        span = size_mask;
    unsigned end = start + span;
    unsigned ma = a & ~mir;
    if (ma < start || ma > end)
        return 0;
    *hit_off = ma - start;
    return 1;
}

static int mapper_lookup(unsigned a, struct map_hit *hit)
{
    unsigned off;
    a &= 0xffffffu;
    for (unsigned i = 0; i < 8; i++) {
        switch (i) {
        case 0:
            if (compute_region(i, 0x40000u, 0xfc0000u, 0, a, &off)) {
                hit->kind = MAP_ROM; hit->backing = off; hit->offset = off; return 1;
            }
            break;
        case 1:
            if (compute_region(i, 0x40000u, 0xfc0000u, 0, a, &off)) {
                hit->kind = MAP_ROM; hit->backing = 0x40000u + off; hit->offset = off; return 1;
            }
            break;
        case 2:
            if (compute_region(i, 0x10000u, 0xff0000u, 0, a, &off)) {
                hit->kind = MAP_TILEBANK; hit->backing = 0; hit->offset = off; return 1;
            }
            break;
        case 3:
            if (compute_region(i, 0x4000u, 0xffc000u, 0, a, &off)) {
                hit->kind = MAP_RAM; hit->backing = 0xffc000u + off; hit->offset = off; return 1;
            }
            break;
        case 4:
            if (compute_region(i, 0x800u, 0xfff800u, 0, a, &off)) {
                hit->kind = MAP_RAM; hit->backing = 0x440000u + off; hit->offset = off; return 1;
            }
            break;
        case 5:
            if (compute_region(i, 0x1000u, 0xfef000u, 0x10000u, a, &off)) {
                hit->kind = MAP_RAM; hit->backing = 0x410000u + off; hit->offset = off; return 1;
            }
            if (compute_region(i, 0x10000u, 0xfe0000u, 0, a, &off)) {
                hit->kind = MAP_RAM; hit->backing = 0x400000u + off; hit->offset = off; return 1;
            }
            break;
        case 6:
            if (compute_region(i, 0x1000u, 0xfff000u, 0, a, &off)) {
                hit->kind = MAP_RAM; hit->backing = 0x840000u + off; hit->offset = off; return 1;
            }
            break;
        case 7:
            if (compute_region(i, 0x4000u, 0xffc000u, 0, a, &off)) {
                hit->kind = MAP_IO; hit->backing = 0; hit->offset = off; return 1;
            }
            break;
        }
    }
    hit->kind = MAP_NONE;
    hit->backing = 0;
    hit->offset = 0;
    return 0;
}

static uint8_t *region_ptr(unsigned a)
{
    a &= 0xffffff;
    if (a >= 0x200000 && a < 0x240000) return gbase + a;
    struct map_hit hit;
    if (mapper_lookup(a, &hit)) {
        if (hit.kind == MAP_ROM || hit.kind == MAP_RAM)
            return gbase + hit.backing;
        return 0;
    }
    if (a < 0x080000) return gbase + a;
    return 0;
}

static unsigned read_bytes(unsigned a, int size)
{
    a &= 0xffffff;
    struct map_hit hit;
    if (mapper_lookup(a, &hit)) {
        if (hit.kind == MAP_IO)
            return io_read_offset(hit.offset, size);
        if (hit.kind == MAP_TILEBANK)
            return size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffffu;
        if (hit.kind == MAP_ROM || hit.kind == MAP_RAM) {
            uint8_t *p = gbase + hit.backing;
            unsigned v = 0;
            for (int i = 0; i < size; i++) v = (v << 8) | p[i];
            return v;
        }
    }
    if (a >= 0xC40000 && a < 0xC44000) return io_read_offset(a - 0xC40000u, size);
    if (a >= 0xC60000 && a < 0xC60002) return size == 1 ? 0xff : 0xffff;
    if (a >= 0x3F0000 && a < 0x400000) return size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffffu;
    if (a >= 0xFE0000 && a < 0xFE0040) {
        unsigned off = ((a - 0xFE0000u) >> 1) & 0x1f;
        unsigned val = 0xff;
        if (off == 0x00 || off == 0x01)
            val = mapper_regs[off];
        else if (off == 0x02)
            val = 0x0f;
        else if (off == 0x03)
            val = shinobi_audio_response();
        return size == 1 ? (val & 0xff) : (0xff00 | (val & 0xff));
    }
    uint8_t *p = region_ptr(a);
    if (!p) return size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffffu;
    unsigned v = 0;
    for (int i = 0; i < size; i++) v = (v << 8) | p[i];
    return v;
}

static void mapper_write(unsigned a, unsigned v, int size)
{
    if (size == 4) {
        mapper_write(a, v >> 16, 2);
        mapper_write(a + 2, v, 2);
        return;
    }

    uint8_t data = (uint8_t)v;
    unsigned off = (a >> 1) & 0x1f;
    uint8_t old = mapper_regs[off];
    mapper_regs[off] = data;

    switch (off) {
    case 0x03:
        shinobi_audio_command(data);
        break;
    case 0x04:
        if ((old ^ data) && ((data & 7) != 7)) {
            int irq = (~data) & 7;
            if (irq)
                m68k_set_irq(irq);
        }
        break;
    case 0x05:
        if (data == 0x01) {
            unsigned wa = ((unsigned)mapper_regs[0x0a] << 17) |
                          ((unsigned)mapper_regs[0x0b] << 9) |
                          ((unsigned)mapper_regs[0x0c] << 1);
            write_bytes(wa, ((unsigned)mapper_regs[0x00] << 8) | mapper_regs[0x01], 2);
        } else if (data == 0x02) {
            unsigned ra = ((unsigned)mapper_regs[0x07] << 17) |
                          ((unsigned)mapper_regs[0x08] << 9) |
                          ((unsigned)mapper_regs[0x09] << 1);
            unsigned rv = read_bytes(ra, 2);
            mapper_regs[0x00] = (uint8_t)(rv >> 8);
            mapper_regs[0x01] = (uint8_t)rv;
        }
        break;
    default:
        break;
    }
}

static void write_bytes(unsigned a, unsigned v, int size)
{
    a &= 0xffffff;
    struct map_hit hit;
    if (mapper_lookup(a, &hit)) {
        if (hit.kind == MAP_ROM)
            return;
        if (hit.kind == MAP_IO) {
            unsigned word_off = (hit.offset >> 1) & 0x1fff;
            if ((word_off & (0x3000 / 2)) == 0) {
                misc_output = (uint8_t)v;
                shinobi_set_screen_flip((misc_output & 0x40) != 0);
            }
            return;
        }
        if (hit.kind == MAP_TILEBANK) {
            set_tile_bank_latch((int)((hit.offset >> 1) & 1u), (int)(v & 7u));
            return;
        }
        if (hit.kind == MAP_RAM) {
            uint8_t *p = gbase + hit.backing;
            int changed = 0;
            for (int i = 0; i < size; i++) {
                uint8_t b = (uint8_t)(v >> ((size - 1 - i) * 8));
                if (p[i] != b)
                    changed = 1;
            }
            if (changed && hit.backing >= 0x400000u && hit.backing < 0x410000u)
                shinobi_mark_tile_dirty(hit.backing);
            if (changed && hit.backing >= 0x840000u && hit.backing < 0x841000u)
                shinobi_mark_palette_dirty();
            for (int i = size - 1; i >= 0; i--) { p[i] = v & 0xff; v >>= 8; }
            return;
        }
    }
    if ((a & ~7u) == 0xC43000)
        return;
    if (a >= 0xC40000 && a < 0xC44000) {
        if (a == 0xC40001 || (a & ~1u) == 0xC40000) {
            misc_output = (uint8_t)v;
            shinobi_set_screen_flip((misc_output & 0x40) != 0);
        }
        return;
    }
    if (a >= 0xC60000 && a < 0xC60002) return;
    if (a >= 0x123406 && a < 0x123408) {
        shinobi_audio_command((uint8_t)v);
        return;
    }
    if (a >= 0x3F0000 && a < 0x400000) {
        set_tile_bank_latch((int)(((a - 0x3F0000u) >> 1) & 1u), (int)(v & 7u));
        return;
    }
    if (a >= 0xFE0000 && a < 0xFE0040) {
        mapper_write(a, v, size);
        return;
    }
    uint8_t *p = region_ptr(a);
    if (!p) return;
    int changed = 0;
    for (int i = 0; i < size; i++) {
        uint8_t b = (uint8_t)(v >> ((size - 1 - i) * 8));
        if (p[i] != b)
            changed = 1;
    }
    if (changed && a >= 0x400000 && a < 0x410000)
        shinobi_mark_tile_dirty(a);
    if (changed && a >= 0x840000 && a < 0x841000)
        shinobi_mark_palette_dirty();
    for (int i = size - 1; i >= 0; i--) { p[i] = v & 0xff; v >>= 8; }
}

unsigned int m68k_read_memory_8(unsigned int a){ return read_bytes(a, 1); }
unsigned int m68k_read_memory_16(unsigned int a){ return read_bytes(a, 2); }
unsigned int m68k_read_memory_32(unsigned int a){ return (m68k_read_memory_16(a)<<16)|m68k_read_memory_16(a+2); }
unsigned int m68k_read_disassembler_8 (unsigned int a){return m68k_read_memory_8(a);}
unsigned int m68k_read_disassembler_16(unsigned int a){return m68k_read_memory_16(a);}
unsigned int m68k_read_disassembler_32(unsigned int a){return m68k_read_memory_32(a);}
void m68k_write_memory_8(unsigned int a, unsigned int v){ write_bytes(a, v, 1); }
void m68k_write_memory_16(unsigned int a, unsigned int v){ write_bytes(a, v, 2); }
void m68k_write_memory_32(unsigned int a, unsigned int v){ m68k_write_memory_16(a,v>>16); m68k_write_memory_16(a+2,v); }

int shinobi_dyntrans_init(void){
    if (!shinobi_assets_load()) return 0;
    gbase = (uint8_t*)AllocMem(GUEST_SIZE, MEMF_FAST|MEMF_CLEAR);
    if (!gbase) gbase = (uint8_t*)AllocMem(GUEST_SIZE, MEMF_PUBLIC|MEMF_CLEAR);
    if (!gbase) return 0;
    for (int i = 0; i < 0x20; i++) mapper_regs[i] = 0;
    tile_bank_regs[0] = 0;
    tile_bank_regs[1] = 1;
    frame_counter = 0;
    shinobi_set_tile_bank(0, tile_bank_regs[0]);
    shinobi_set_tile_bank(1, tile_bank_regs[1]);
    for (long i=0;i<0x80000;i++) gbase[i] = shinobi_rom_main[i];   /* ROM -> guest $0 */
    m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_set_int_ack_callback(int_ack);
    m68k_pulse_reset();
    return 1;
}
void shinobi_dyntrans_set_inputs(uint8_t p1, uint8_t p2, uint8_t svc, uint8_t d1, uint8_t d2){
    in_p1 = p1; in_p2 = p2; in_service = svc; dsw1 = d1; dsw2 = d2;
}
static int frame_rendered = 1;
int shinobi_dyntrans_rendered(void) { return frame_rendered; }
void shinobi_dyntrans_frame(void){
    int remaining = 166666;   /* full 10MHz/60fps System16B frame (was 90000 = ~54% speed) */
    m68k_set_irq(4);
    while (remaining > 0) {
        int slice = remaining > 20000 ? 20000 : remaining;
        int used = m68k_execute(slice);
        remaining -= used > 0 ? used : slice;
    }
    m68k_set_irq(0);
    frame_rendered = ((frame_counter++ & 1u) == 0);
    if (frame_rendered)
        shinobi_render(gbase);
}
