/*
 * shinobi_host.c — host-side Musashi 68000 REFERENCE harness for Shinobi
 * (Sega System 16B, shinobi6 set).  This is the GOLDEN ORACLE that the
 * future rebase-recompiled Amiga build must match.
 *
 * It loads the flat 256KB program image (games/shinobi/roms/shinobi_main.bin),
 * implements the post-mapper System 16B memory map with RAM regions backed by
 * arrays and MMIO redirected to logging HAL stubs, seeds SSP/PC from the reset
 * vectors, and runs ~120 frames with a 60Hz IRQ4 (vblank) raised each frame.
 *
 * It proves the boot behaviour: mapper programming, IRQ4 service, and writes
 * to tile/text/sprite/palette RAM.
 *
 * Build:
 *   cc -O2 -I src/cores/m68k -o /tmp/shinobi_host \
 *       tools/shinobi_host.c \
 *       src/cores/m68k/m68kcpu.c src/cores/m68k/m68kops.c src/cores/m68k/m68kdasm.c
 * Run:
 *   /tmp/shinobi_host games/shinobi/roms/shinobi_main.bin [frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "m68k.h"

/* ---- backing RAM regions (post-mapper layout) ---- */
static uint8_t ROM[0x40000];      /* 0x000000-0x03FFFF program (256KB)         */
static uint8_t TILERAM[0x10000];  /* 0x400000-0x40FFFF tilemap layers          */
static uint8_t TEXTRAM[0x1000];   /* 0x410000-0x410FFF text layer + scroll regs */
static uint8_t SPRRAM[0x800];     /* 0x440000-0x4407FF sprite object list       */
static uint8_t PALRAM[0x1000];    /* 0x840000-0x840FFF palette RAM              */
static uint8_t WORKRAM[0x10000];  /* 0xFF0000-0xFFFFFF work RAM (stack @0xFFFF00)*/

/* ---- instrumentation ---- */
static long g_irq4_raised = 0;
static long g_irq4_serviced = 0;   /* times PC actually entered handler 0x2684 */
static long g_mapper_writes = 0;
static long g_watchdog_kicks = 0;
static long g_sound_latch_writes = 0;
static long g_mapper_sound_writes = 0;
static long g_cmd_window_writes = 0;   /* 0x3F0000 region (movep sound pulse) */
static long g_misc_out_writes = 0;
static long g_io_reads = 0;
static int  g_first_tile = -1, g_first_text = -1, g_first_spr = -1, g_first_pal = -1;
static uint8_t g_mapper_table[16];
static int  g_mapper_have = 0;
static int  g_log_io = 0;          /* verbose MMIO logging during first frame */

static uint8_t last_sound_latch = 0;
static uint8_t sound_latch_seen[256];
static uint8_t mapper_sound_seen[256];

/* default not-pressed inputs (active low) and DSW defaults */
static uint8_t in_service = 0xff, in_p1 = 0xff, in_p2 = 0xff;
static uint8_t dsw1 = 0xff, dsw2 = 0xff;

/* ---- MMIO handlers ---- */
static unsigned io_read(unsigned addr, int size)
{
    g_io_reads++;
    /* 0xC40000 region, custom_io -> standard_io_r.  offset = (addr-base)>>1 word offset */
    unsigned word_off = ((addr - 0xC40000) >> 1) & 0x1fff;
    unsigned val = 0xffff;
    switch (word_off & (0x3000/2)) {
        case 0x1000/2: {
            /* sysports[] = { SERVICE, P1, UNUSED, P2 } indexed by word_off&3 */
            switch (word_off & 3) {
                case 0: val = in_service; break;
                case 1: val = in_p1; break;
                case 2: val = 0xff; break;       /* UNUSED */
                case 3: val = in_p2; break;
            }
            break;
        }
        case 0x2000/2:
            val = (word_off & 1) ? dsw1 : dsw2;
            break;
        default:
            /* open-bus / unknown */
            val = 0xffff;
            break;
    }
    if (g_log_io)
        fprintf(stderr, "  [io_r ] %06x sz%d -> %02x (woff %04x)\n", addr, size, val & 0xff, word_off);
    /* I/O lives on the low data byte; word reads see 0xff in high byte. */
    if (size == 1) return val & 0xff;
    return 0xff00 | (val & 0xff);
}

static void io_write(unsigned addr, unsigned val, int size)
{
    /* decode by exact byte address (boot uses byte writes) */
    if (addr == 0xC40001 || (addr & ~1) == 0xC40000) {
        g_misc_out_writes++;
        if (g_log_io)
            fprintf(stderr, "  [misc_out] %06x = %02x (D6flip=%d D5disp=%d coin=%d)\n",
                    addr, val & 0xff, !!(val&0x40), !!(val&0x20), val & 0x03);
        return;
    }
    if ((addr & ~1) == 0xC43000) {  /* 0xC43001 sound latch */
        g_sound_latch_writes++;
        last_sound_latch = val & 0xff;
        sound_latch_seen[last_sound_latch] = 1;
        if (g_log_io)
            fprintf(stderr, "  [sndlatch] %06x = %02x\n", addr, val & 0xff);
        return;
    }
    if ((addr & ~7) == 0xC43000) {  /* 0xC43007 misc ctrl */
        if (g_log_io)
            fprintf(stderr, "  [miscctrl] %06x = %02x\n", addr, val & 0xff);
        return;
    }
    if (g_log_io)
        fprintf(stderr, "  [io_w ] %06x = %0*x sz%d\n", addr, size*2, val, size);
}

/* ---- memory dispatch ---- */
static uint8_t *region_ptr(unsigned addr, unsigned *limit)
{
    addr &= 0xFFFFFF;
    if (addr < 0x040000)                       { *limit = 0x040000; return ROM + addr; }
    if (addr >= 0x400000 && addr < 0x410000)   { *limit = 0x010000; return TILERAM + (addr-0x400000); }
    if (addr >= 0x410000 && addr < 0x411000)   { *limit = 0x001000; return TEXTRAM + (addr-0x410000); }
    if (addr >= 0x440000 && addr < 0x440800)   { *limit = 0x000800; return SPRRAM  + (addr-0x440000); }
    if (addr >= 0x840000 && addr < 0x841000)   { *limit = 0x001000; return PALRAM  + (addr-0x840000); }
    if (addr >= 0xFF0000)                       { *limit = 0x010000; return WORKRAM + (addr-0xFF0000); }
    return NULL;
}

static unsigned read_bytes(unsigned addr, int size)
{
    addr &= 0xFFFFFF;
    uint8_t *p; unsigned limit;
    /* MMIO regions first */
    if (addr >= 0xC40000 && addr < 0xC44000) return io_read(addr, size);
    if (addr >= 0xC60000 && addr < 0xC60002) { g_watchdog_kicks++; return 0xffff; } /* watchdog read */
    if (addr >= 0x3F0000 && addr < 0x400000) { return 0xffff; } /* cmd window read (open bus) */
    if (addr >= 0xFE0000 && addr < 0xFE0040) { return 0xffff; } /* mapper regs read */
    p = region_ptr(addr, &limit);
    if (!p) {
        if (g_log_io) fprintf(stderr, "  [r? ] unmapped %06x sz%d\n", addr, size);
        return (size==1)?0xff:(size==2)?0xffff:0xffffffff;
    }
    unsigned v = 0;
    for (int i=0;i<size;i++) v = (v<<8) | p[i];
    return v;
}

static void write_bytes(unsigned addr, unsigned val, int size)
{
    addr &= 0xFFFFFF;
    uint8_t *p; unsigned limit;
    if (addr >= 0xC40000 && addr < 0xC44000) { io_write(addr, val, size); return; }
    if (addr >= 0xC60000 && addr < 0xC60002) { g_watchdog_kicks++; return; }
    if (addr >= 0x3F0000 && addr < 0x400000) {  /* 315-5195 cmd/sound window (movep) */
        g_cmd_window_writes++;
        if (g_log_io) fprintf(stderr, "  [cmdwin ] %06x = %0*x sz%d\n", addr, size*2, val, size);
        return;
    }
    if (addr >= 0xFE0000 && addr < 0xFE0040) {  /* mapper registers */
        g_mapper_writes++;
        if ((((addr - 0xFE0000) >> 1) & 0x1f) == 0x03) {
            g_mapper_sound_writes++;
            mapper_sound_seen[val & 0xff] = 1;
        }
        /* boot writes 16 WORD entries to 0xFE0020 step 2; table byte in low byte */
        if (addr >= 0xFE0020 && addr < 0xFE0040) {
            unsigned idx = (addr - 0xFE0020) >> 1;
            if (idx < 16) { g_mapper_table[idx] = val & 0xff; if (idx==15) g_mapper_have = 1; }
        }
        return;
    }
    p = region_ptr(addr, &limit);
    if (!p) { if (g_log_io) fprintf(stderr,"  [w? ] unmapped %06x=%x sz%d\n",addr,val,size); return; }
    for (int i=size-1;i>=0;i--) { p[i] = val & 0xff; val >>= 8; }

    /* first-write tracking */
    if (addr >= 0x400000 && addr < 0x410000 && g_first_tile<0) g_first_tile = addr;
    if (addr >= 0x410000 && addr < 0x411000 && g_first_text<0) g_first_text = addr;
    if (addr >= 0x440000 && addr < 0x440800 && g_first_spr <0) g_first_spr  = addr;
    if (addr >= 0x840000 && addr < 0x841000 && g_first_pal <0) g_first_pal  = addr;
}

/* ---- Musashi memory callbacks ---- */
unsigned int m68k_read_memory_8 (unsigned int a){ return read_bytes(a,1); }
unsigned int m68k_read_memory_16(unsigned int a){ return read_bytes(a,2); }
unsigned int m68k_read_memory_32(unsigned int a){ return read_bytes(a,4); }
void m68k_write_memory_8 (unsigned int a, unsigned int v){ write_bytes(a,v,1); }
void m68k_write_memory_16(unsigned int a, unsigned int v){ write_bytes(a,v,2); }
void m68k_write_memory_32(unsigned int a, unsigned int v){ write_bytes(a,v,4); }
/* immediate/disassembler reads (program fetch) */
unsigned int m68k_read_disassembler_8 (unsigned int a){ return read_bytes(a,1); }
unsigned int m68k_read_disassembler_16(unsigned int a){ return read_bytes(a,2); }
unsigned int m68k_read_disassembler_32(unsigned int a){ return read_bytes(a,4); }

/* int-ack: emulate irq4_line_hold — line auto-clears once the CPU takes it. */
static int int_ack(int level){ m68k_set_irq(0); return M68K_INT_ACK_AUTOVECTOR; }

/* instruction hook: detect actual entry into the IRQ4 handler at 0x2684 */
static void instr_hook(unsigned int pc){ if (pc==0x2684) g_irq4_serviced++; }

static void dump_state(void)
{
    fprintf(stderr, "\n=== POST-RUN STATE ===\n");
    fprintf(stderr, "IRQ4 raised/served: %ld / %ld (handler 0x2684 entries)\n",
            g_irq4_raised, g_irq4_serviced);
    fprintf(stderr, "mapper reg writes : %ld (16-byte region table %s)\n",
            g_mapper_writes, g_mapper_have?"fully captured":"captured");
    fprintf(stderr, "mapper table      :");
    for (int i=0;i<16;i++) fprintf(stderr," %02x", g_mapper_table[i]);
    fprintf(stderr, "\n");
    fprintf(stderr, "watchdog kicks    : %ld\n", g_watchdog_kicks);
    fprintf(stderr, "sound-latch writes: %ld (last=%02x)\n", g_sound_latch_writes, last_sound_latch);
    fprintf(stderr, "sound-latch values:");
    for (int i=0;i<256;i++) if (sound_latch_seen[i]) fprintf(stderr, " %02x", i);
    fprintf(stderr, "\n");
    fprintf(stderr, "mapper sound wrs  : %ld\n", g_mapper_sound_writes);
    fprintf(stderr, "mapper sound vals :");
    for (int i=0;i<256;i++) if (mapper_sound_seen[i]) fprintf(stderr, " %02x", i);
    fprintf(stderr, "\n");
    fprintf(stderr, "cmd-window writes : %ld (0x3F0001 sound/IRQ pulse)\n", g_cmd_window_writes);
    fprintf(stderr, "misc-output writes: %ld (0xC40001 coin/lamp/disp/flip)\n", g_misc_out_writes);
    fprintf(stderr, "I/O port reads    : %ld\n", g_io_reads);

    fprintf(stderr, "\nFirst writes:\n");
    fprintf(stderr, "  tileram   : %s%06x\n", g_first_tile<0?"(none) ":"",   g_first_tile<0?0:g_first_tile);
    fprintf(stderr, "  textram   : %s%06x\n", g_first_text<0?"(none) ":"",   g_first_text<0?0:g_first_text);
    fprintf(stderr, "  spriteram : %s%06x\n", g_first_spr <0?"(none) ":"",   g_first_spr <0?0:g_first_spr);
    fprintf(stderr, "  paletteram: %s%06x\n", g_first_pal <0?"(none) ":"",   g_first_pal <0?0:g_first_pal);

    /* text scroll registers written by IRQ4 (0x410E90/92/98/9A) */
    #define TX16(off) ((TEXTRAM[(off)-0x410000]<<8)|TEXTRAM[(off)-0x410000+1])
    fprintf(stderr, "\nText scroll regs (written by IRQ4 @0x2684):\n");
    fprintf(stderr, "  0x410E90=%04x 0x410E92=%04x 0x410E98=%04x 0x410E9A=%04x\n",
            TX16(0x410E90), TX16(0x410E92), TX16(0x410E98), TX16(0x410E9A));

    /* sprite list: scan for non-zero entries and the 0xffff terminator */
    int spr_nz=0, spr_term=-1;
    for (int i=0;i<0x800;i+=2) {
        if (SPRRAM[i]||SPRRAM[i+1]) spr_nz++;
        if (spr_term<0 && SPRRAM[i]==0xff && SPRRAM[i+1]==0xff) spr_term=0x440000+i;
    }
    fprintf(stderr, "\nSprite list: %d/%d non-zero words; 0xffff terminator at %s%06x\n",
            spr_nz, 0x800/2, spr_term<0?"(none) ":"", spr_term<0?0:spr_term);
    fprintf(stderr, "  first 16 sprite words @0x440000:");
    for (int i=0;i<16;i++) fprintf(stderr," %02x%02x", SPRRAM[i*2], SPRRAM[i*2+1]);
    fprintf(stderr, "\n");

    /* palette: count non-zero entries + show a few */
    int pal_nz=0; for (int i=0;i<0x1000;i+=2) if (PALRAM[i]||PALRAM[i+1]) pal_nz++;
    fprintf(stderr, "\nPalette: %d/%d non-zero entries\n", pal_nz, 0x1000/2);
    fprintf(stderr, "  entries @0x840000:");
    for (int i=0;i<12;i++) fprintf(stderr," %02x%02x", PALRAM[i*2], PALRAM[i*2+1]);
    fprintf(stderr, "\n");

    /* tilemap: count non-zero words */
    int tile_nz=0; for (int i=0;i<0x10000;i+=2) if (TILERAM[i]||TILERAM[i+1]) tile_nz++;
    fprintf(stderr, "\nTilemap: %d/%d non-zero words @0x400000\n", tile_nz, 0x10000/2);
    int text_nz=0; for (int i=0;i<0x1000;i+=2) if (TEXTRAM[i]||TEXTRAM[i+1]) text_nz++;
    fprintf(stderr, "Textram: %d/%d non-zero words @0x410000\n", text_nz, 0x1000/2);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr,"usage: %s shinobi_main.bin [frames]\n",argv[0]); return 1; }
    int frames = (argc>=3)?atoi(argv[2]):120;

    FILE *f = fopen(argv[1],"rb");
    if (!f){ perror("fopen"); return 1; }
    size_t n = fread(ROM,1,sizeof ROM,f); fclose(f);
    fprintf(stderr,"loaded %zu bytes of program ROM\n", n);

    unsigned ssp = (ROM[0]<<24)|(ROM[1]<<16)|(ROM[2]<<8)|ROM[3];
    unsigned pc  = (ROM[4]<<24)|(ROM[5]<<16)|(ROM[6]<<8)|ROM[7];
    fprintf(stderr,"reset SSP=%08x  PC=%08x\n", ssp, pc);
    fprintf(stderr,"IRQ4 vector (0x70)=%08x\n",
            (ROM[0x70]<<24)|(ROM[0x71]<<16)|(ROM[0x72]<<8)|ROM[0x73]);

    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_init();
    m68k_set_int_ack_callback(int_ack);
    m68k_set_instr_hook_callback(instr_hook);
    m68k_pulse_reset();   /* reads SSP/PC from our memory at 0/4 */

    /* System 16B main CPU ~10MHz, 60Hz vblank */
    const int CYC_PER_FRAME = 10000000/60;
    g_log_io = 1;  /* verbose for frame 0 to capture the boot sequence */

    for (int fr=0; fr<frames; fr++) {
        if (fr==1) g_log_io = 0;   /* quiet after first frame */
        if (fr >= 120 && fr < 130) in_service &= (uint8_t)~0x01; else in_service |= 0x01;
        if (fr >= 150 && fr < 174) in_service &= (uint8_t)~0x10; else in_service |= 0x10;
        m68k_set_irq(4);           /* raise vblank IRQ4 */
        g_irq4_raised++;
        int remaining = CYC_PER_FRAME;
        while (remaining > 0) remaining -= m68k_execute(remaining>20000?20000:remaining);
        m68k_set_irq(0);
        if (fr==0) fprintf(stderr,"--- end of frame 0 ---\n");
    }

    fprintf(stderr,"\nran %d frames, final PC=%06x\n", frames,
            m68k_get_reg(NULL,M68K_REG_PC));
    dump_state();
    return 0;
}
