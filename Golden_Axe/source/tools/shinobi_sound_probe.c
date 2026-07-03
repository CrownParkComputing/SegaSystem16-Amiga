#include "../cores/z80emu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static MY_LITTLE_Z80 z80;
static uint8_t snd[0x8000];
static uint8_t sample[0x20000];
static uint8_t latch;
static int latch_irq;
static uint8_t ym_addr, ym_regs[256], ym_status;
static int ym_irqen_a, ym_irqen_b, ym_ta_load, ym_tb_load;
static long ym_ta_count, ym_tb_count;
static unsigned ym_addr_w, ym_data_w, keyons, sample_w;

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
            ym_data_w++;
            ym_regs[ym_addr] = x;
            if (ym_addr == 0x08 && (x & 0x78)) keyons++;
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
            ym_addr_w++;
            ym_addr = x;
        }
        return;
    }
    if ((port & 0xc0) == 0x40) {
        install_bank(x);
        return;
    }
    if ((port & 0xc0) == 0x80) {
        sample_w++;
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
        n -= did;
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s epr-11361.a10 mpr-11362.a11\n", argv[0]);
        return 2;
    }
    if (!load_file(argv[1], snd, sizeof snd) || !load_file(argv[2], sample, sizeof sample)) {
        fprintf(stderr, "failed to load sound ROMs\n");
        return 1;
    }
    memset(&z80, 0, sizeof z80);
    memcpy(z80.memory, snd, sizeof snd);
    install_bank(0);
    Z80Reset(&z80.state);
    run_cycles(500000);
    printf("after boot pc=%04x ym_addr=%u ym_data=%u keyons=%u sample=%u\n",
           z80.state.pc & 0xffff, ym_addr_w, ym_data_w, keyons, sample_w);

    for (unsigned cmd = 1; cmd < 0x100; cmd++) {
        unsigned a0 = ym_addr_w, d0 = ym_data_w, k0 = keyons, s0 = sample_w;
        latch = (uint8_t)cmd;
        latch_irq = 1;
        run_cycles(200000);
        if (ym_addr_w != a0 || ym_data_w != d0 || keyons != k0 || sample_w != s0) {
            printf("cmd %02x pc=%04x ym_addr +%u ym_data +%u keyons +%u sample +%u\n",
                   cmd, z80.state.pc & 0xffff, ym_addr_w-a0, ym_data_w-d0, keyons-k0, sample_w-s0);
        }
    }
    printf("final pc=%04x ym_addr=%u ym_data=%u keyons=%u sample=%u\n",
           z80.state.pc & 0xffff, ym_addr_w, ym_data_w, keyons, sample_w);
    return 0;
}
