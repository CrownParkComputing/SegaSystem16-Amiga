#include "../cores/z80emu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Timeline probe: run ONE sound command for several seconds and log every
 * 0x40 bank/sample-trigger write (the real port's direct_pcm intercept point)
 * and every YM key-on, timestamped in seconds since the command was latched.
 * Reveals whether a command fires a sample immediately AND again ~1s later
 * (the user's "crackle then plays it a second later" double-event). */

static MY_LITTLE_Z80 z80;
static uint8_t snd[0x8000];
static uint8_t sample[0x20000];
static uint8_t latch;
static int latch_irq;
static uint8_t ym_addr, ym_regs[256], ym_status;
static int ym_irqen_a, ym_irqen_b, ym_ta_load, ym_tb_load;
static long ym_ta_count, ym_tb_count;
static long long total_cycles;          /* cycles since command latch */
static int logging;

static int load_file(const char *path, uint8_t *dst, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t got = fread(dst, 1, n, f);
    fclose(f);
    return got == n;
}

static void install_bank(uint8_t data)
{
    unsigned bankoffs = ((data & 0x08u) >> 3) * 0x20000u;
    bankoffs += (data & 0x07u) * 0x4000u;
    bankoffs %= 0x20000u;
    memcpy(z80.memory + 0x8000, sample + bankoffs, 0x6000);
    if (logging) {
        double t = total_cycles / 5000000.0;
        unsigned absbank = ((data & 0x08u) >> 3) * 0x20000u + (data & 0x07u) * 0x4000u;
        printf("  %7.4fs  0x40 write=%02x  (rombank=%05x)  pc=%04x\n",
               t, data, absbank, z80.state.pc & 0xffff);
    }
}

unsigned char machine_rd(MY_LITTLE_Z80 *z, unsigned int addr)
{
    (void)z;
    addr &= 0xffff;
    if (addr == 0xe800) {
        latch_irq = 0;
        return latch;
    }
    return z80.memory[addr];
}

void machine_wr(MY_LITTLE_Z80 *z, unsigned int addr, unsigned char val)
{
    (void)z;
    addr &= 0xffff;
    if (addr >= 0xf800)
        z80.memory[addr] = val;
}

unsigned char in_impl(MY_LITTLE_Z80 *z, int port)
{
    (void)z;
    port &= 0xff;
    if ((port & 0xc0) == 0x00)
        return ym_status;
    if ((port & 0xc0) == 0xc0) {
        latch_irq = 0;
        return latch;
    }
    if ((port & 0xc0) == 0x80)
        return 0;
    return 0xff;
}

void out_impl(MY_LITTLE_Z80 *z, int port, unsigned char x)
{
    (void)z;
    port &= 0xff;
    if ((port & 0xc0) == 0x00) {
        if (port & 1) {
            ym_regs[ym_addr] = x;
            if (ym_addr == 0x08 && (x & 0x78)) {
                int ch = 0;
                switch (x & 0x78) {
                case 0x78: ch = 0; break;
                case 0x70: ch = 1; break;
                case 0x68: ch = 2; break;
                case 0x60: ch = 3; break;
                case 0x58: ch = 4; break;
                case 0x50: ch = 5; break;
                case 0x48: ch = 6; break;
                case 0x40: ch = 7; break;
                }
                if (logging) {
                    double t = total_cycles / 5000000.0;
                    printf("  %7.4fs  YM keyon ch%d  pc=%04x\n", t, ch, z80.state.pc & 0xffff);
                }
            }
            if (ym_addr == 0x14) {
                ym_irqen_a = (x >> 2) & 1;
                ym_irqen_b = (x >> 3) & 1;
                if (x & 0x01) {
                    int ta = (ym_regs[0x10] << 2) | (ym_regs[0x11] & 3);
                    ym_ta_count = (long)(1024 - ta) * 128;
                    ym_ta_load = 1;
                } else ym_ta_load = 0;
                if (x & 0x02) {
                    int tb = ym_regs[0x12];
                    ym_tb_count = (long)(256 - tb) * 2048;
                    ym_tb_load = 1;
                } else ym_tb_load = 0;
                if (x & 0x10) ym_status &= ~0x01;
                if (x & 0x20) ym_status &= ~0x02;
            }
        } else {
            ym_addr = x;
        }
        return;
    }
    if ((port & 0xc0) == 0x40) {
        install_bank(x);
        return;
    }
    if ((port & 0xc0) == 0x80) {
        if (logging) {
            double t = total_cycles / 5000000.0;
            printf("  %7.4fs  0x80 write=%02x  pc=%04x\n", t, x, z80.state.pc & 0xffff);
        }
        return;
    }
}

static void ym_advance(int cycles)
{
    if (ym_ta_load) {
        ym_ta_count -= cycles;
        while (ym_ta_count <= 0) {
            ym_status |= 0x01;
            int ta = (ym_regs[0x10] << 2) | (ym_regs[0x11] & 3);
            ym_ta_count += (long)(1024 - ta) * 128;
        }
    }
    if (ym_tb_load) {
        ym_tb_count -= cycles;
        while (ym_tb_count <= 0) {
            ym_status |= 0x02;
            int tb = ym_regs[0x12];
            ym_tb_count += (long)(256 - tb) * 2048;
        }
    }
}

static void run_cycles(int n)
{
    while (n > 0) {
        int slice = n > 512 ? 512 : n;
        if (latch_irq)
            Z80Interrupt(&z80.state, 0xff, &z80);
        int did = Z80Emulate(&z80.state, slice, &z80);
        if (did <= 0) did = slice;
        ym_advance(did);
        total_cycles += did;
        n -= did;
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s epr-11361.a10 mpr-11362.a11 CMD [seconds]\n", argv[0]);
        return 2;
    }
    if (!load_file(argv[1], snd, sizeof snd) || !load_file(argv[2], sample, sizeof sample)) {
        fprintf(stderr, "failed to load sound ROMs\n");
        return 1;
    }
    double secs = argc > 4 ? atof(argv[4]) : 4.0;
    unsigned cmd = (unsigned)strtoul(argv[3], 0, 0);

    memset(&z80, 0, sizeof z80);
    memcpy(z80.memory, snd, sizeof snd);
    install_bank(0);
    Z80Reset(&z80.state);
    logging = 0;
    total_cycles = 0;
    run_cycles(500000);                 /* boot */

    printf("=== command 0x%02x for %.1fs ===\n", cmd, secs);
    latch = (uint8_t)cmd;
    latch_irq = 1;
    logging = 1;
    total_cycles = 0;
    run_cycles((int)(secs * 5000000));
    logging = 0;
    printf("=== end ===\n");
    return 0;
}