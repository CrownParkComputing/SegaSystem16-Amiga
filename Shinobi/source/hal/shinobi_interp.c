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
#include <proto/exec.h>
#include <stdint.h>
#include "m68k.h"
#include "shinobi_assets.h"

#define GUEST_SIZE 0x1000000u                /* full 24-bit guest space, flat */
extern void shinobi_render(uint8_t *gbase);
extern void shinobi_set_screen_flip(int flip);
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

static int int_ack(int level)
{
    (void)level;
    m68k_set_irq(0);
    return M68K_INT_ACK_AUTOVECTOR;
}

static unsigned io_read(unsigned a, int size)
{
    unsigned word_off = ((a - 0xC40000) >> 1) & 0x1fff;
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

static uint8_t *region_ptr(unsigned a)
{
    a &= 0xffffff;
    if (a < 0x040000) return gbase + a;
    if (a >= 0x400000 && a < 0x410000) return gbase + a;
    if (a >= 0x410000 && a < 0x411000) return gbase + a;
    if (a >= 0x440000 && a < 0x440800) return gbase + a;
    if (a >= 0x840000 && a < 0x841000) return gbase + a;
    if (a >= 0xff0000) return gbase + a;
    return 0;
}

static unsigned read_bytes(unsigned a, int size)
{
    a &= 0xffffff;
    if (a >= 0xC40000 && a < 0xC44000) return io_read(a, size);
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
    unsigned off = ((a - 0xFE0000u) >> 1) & 0x1f;
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
    default:
        break;
    }
}

static void write_bytes(unsigned a, unsigned v, int size)
{
    a &= 0xffffff;
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
    if (a >= 0x3F0000 && a < 0x400000) {
        shinobi_audio_pulse((uint8_t)v);
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
    for (long i=0;i<0x40000;i++) gbase[i] = shinobi_rom_main[i];   /* ROM -> guest $0 */
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
    int remaining = 90000;
    static unsigned frame_counter;
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
