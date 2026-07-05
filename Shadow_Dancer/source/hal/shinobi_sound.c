/* Shinobi System 16B sound board: Z80 + YM2151 + uPD7759, mixed to Paula. */
#include "z80emu.h"
#include "shinobi_assets.h"
#include <stdint.h>
#include <string.h>

#define Z80_CLOCK       5000000
#define OUT_RATE        22050
#define Z80_AUDIO_QUANTUM 512
#define SHINOBI_UPD_ENABLED 1
#define SHINOBI_UPD_PCM_ENABLED 0
#ifndef SHINOBI_AUDIO_RUST
#define SHINOBI_AUDIO_RUST 1
#endif
#define DIRECT_PCM_RING_SIZE 32768u
#define DIRECT_PCM_RING_MASK (DIRECT_PCM_RING_SIZE - 1u)
#define DIRECT_PCM_CACHE_SLOTS 20u
#define DIRECT_PCM_CACHE_MAX 32768u
#define DIRECT_PCM_MIX_NUM 3
#define DIRECT_PCM_MIX_DEN 5
#define UPD_CLOCK 640000u

extern void shinobi_ym2151_reset(void);
extern void shinobi_ym2151_write_addr(uint8_t v);
extern void shinobi_ym2151_write_data(uint8_t v);
extern uint8_t shinobi_ym2151_read_status(void);
extern int shinobi_ym2151_sample(void);
#if SHINOBI_AUDIO_RUST
extern void run(void *state);
#endif

#if !SHINOBI_AUDIO_RUST
static MY_LITTLE_Z80 z80;
#endif
#if SHINOBI_AUDIO_RUST
static uint8_t aud_state[48];
static uint8_t audmem[0x10000];
#define A8(o)      (aud_state[o])
#define A16(o)     ((uint16_t)((aud_state[o] << 8) | aud_state[(o) + 1]))
#define ASET16(o,v) do { aud_state[o] = (uint8_t)((v) >> 8); aud_state[(o) + 1] = (uint8_t)(v); } while (0)
#define ASET32(o,v) do { unsigned long _v = (unsigned long)(v); aud_state[o] = (uint8_t)(_v >> 24); aud_state[(o) + 1] = (uint8_t)(_v >> 16); aud_state[(o) + 2] = (uint8_t)(_v >> 8); aud_state[(o) + 3] = (uint8_t)_v; } while (0)
#define A_SP       12
#define A_PC       14
#define A_IFF1     26
#define A_IFF2     27
#define A_R        25
#define A_HALTED   29
#define A_CYCLES   32
#define A_BUDGET   36
#endif
static int audio_ready;
static uint8_t latch;
static uint8_t response;
static uint8_t dbg_last_command;
static uint8_t last_accepted_command;
static int latch_irq;
static int latch_pending;
static int command_guard_samples;
static uint8_t ym_addr;
static uint8_t ym_regs[256];
static uint8_t ym_status;
static int ym_irqen_a, ym_irqen_b;
static long ym_ta_count, ym_tb_count;
static int ym_ta_load, ym_tb_load;
static int ym_irq_asserted;
static unsigned dbg_ym_writes, dbg_keyons, dbg_sample_writes;
static unsigned dbg_commands, dbg_high_commands, dbg_pcm_nonzero;
static uint8_t dbg_sample_bytes[64];
static signed char direct_pcm_ring[DIRECT_PCM_RING_SIZE];
static unsigned direct_pcm_rd, direct_pcm_wr, direct_pcm_phase;
static int direct_pcm_sample, direct_pcm_state;

typedef struct {
    uint32_t key;
    unsigned len;
    signed char pcm[DIRECT_PCM_CACHE_MAX];
} direct_pcm_cache_t;

typedef struct {
    signed char *out;
    unsigned len;
    unsigned max;
    unsigned phase;
    int sample;
    int state;
} pcm_builder_t;

static direct_pcm_cache_t direct_pcm_cache[DIRECT_PCM_CACHE_SLOTS];
static unsigned direct_pcm_cache_next;

enum {
    UPD_IDLE,
    UPD_DROP_DRQ,
    UPD_START,
    UPD_FIRST_REQ,
    UPD_LAST_SAMPLE,
    UPD_DUMMY1,
    UPD_ADDR_MSB,
    UPD_ADDR_LSB,
    UPD_DUMMY2,
    UPD_BLOCK_HEADER,
    UPD_NIBBLE_COUNT,
    UPD_NIBBLE_MSN,
    UPD_NIBBLE_LSN
};

static const int upd_step[16][16] = {
    { 0,  0,  1,  2,  3,   5,   7,  10,  0,   0,  -1,  -2,  -3,   -5,   -7,  -10 },
    { 0,  1,  2,  3,  4,   6,   8,  13,  0,  -1,  -2,  -3,  -4,   -6,   -8,  -13 },
    { 0,  1,  2,  4,  5,   7,  10,  15,  0,  -1,  -2,  -4,  -5,   -7,  -10,  -15 },
    { 0,  1,  3,  4,  6,   9,  13,  19,  0,  -1,  -3,  -4,  -6,   -9,  -13,  -19 },
    { 0,  2,  3,  5,  8,  11,  15,  23,  0,  -2,  -3,  -5,  -8,  -11,  -15,  -23 },
    { 0,  2,  4,  7, 10,  14,  19,  29,  0,  -2,  -4,  -7, -10,  -14,  -19,  -29 },
    { 0,  3,  5,  8, 12,  16,  22,  33,  0,  -3,  -5,  -8, -12,  -16,  -22,  -33 },
    { 1,  4,  7, 10, 15,  20,  29,  43, -1,  -4,  -7, -10, -15,  -20,  -29,  -43 },
    { 1,  4,  8, 13, 18,  25,  35,  53, -1,  -4,  -8, -13, -18,  -25,  -35,  -53 },
    { 1,  6, 10, 16, 22,  31,  43,  64, -1,  -6, -10, -16, -22,  -31, -43,  -64 },
    { 2,  7, 12, 19, 27,  37,  51,  76, -2,  -7, -12, -19, -27,  -37, -51,  -76 },
    { 2,  9, 16, 24, 34,  46,  64,  96, -2,  -9, -16, -24, -34,  -46, -64,  -96 },
    { 3, 11, 19, 29, 41,  57,  79, 117, -3, -11, -19, -29, -41,  -57, -79, -117 },
    { 4, 13, 24, 36, 50,  69,  96, 143, -4, -13, -24, -36, -50,  -69, -96, -143 },
    { 4, 16, 29, 44, 62,  85, 118, 175, -4, -16, -29, -44, -62,  -85,-118, -175 },
    { 6, 20, 36, 54, 76, 104, 144, 214, -6, -20, -36, -54, -76, -104,-144, -214 },
};
static const int upd_state_delta[16] = { -1, -1, 0, 0, 1, 2, 2, 3, -1, -1, 0, 0, 1, 2, 2, 3 };

static int upd_state, upd_post_state, upd_clocks_left, upd_post_clocks;
static int upd_drq, upd_nmi_pending, upd_reset, upd_md, upd_mode_slave;
static int upd_waiting_for_data;
static int upd_servicing_drq;
static int upd_nibbles_left, upd_repeat_count, upd_req_sample, upd_last_sample;
static int upd_block_header, upd_sample_rate, upd_first_valid_header;
static unsigned upd_offset, upd_repeat_offset;
static int upd_adpcm_state, upd_adpcm_data, upd_sample;
static int upd_output_active;
static uint8_t upd_fifo;
static int z80_cycle_acc;
static uint32_t z80_rate_acc;

static unsigned direct_pcm_used(void)
{
    return (direct_pcm_wr - direct_pcm_rd) & DIRECT_PCM_RING_MASK;
}

static void direct_pcm_reset(void)
{
    direct_pcm_rd = direct_pcm_wr = direct_pcm_phase = 0;
    direct_pcm_sample = direct_pcm_state = 0;
}

static void direct_pcm_queue(signed char s)
{
    unsigned next = (direct_pcm_wr + 1u) & DIRECT_PCM_RING_MASK;
    if (next == direct_pcm_rd)
        return;
    direct_pcm_ring[direct_pcm_wr] = s;
    direct_pcm_wr = next;
}

static int direct_pcm_pop(void)
{
    if (direct_pcm_rd == direct_pcm_wr)
        return 0;
    int s = direct_pcm_ring[direct_pcm_rd];
    direct_pcm_rd = (direct_pcm_rd + 1u) & DIRECT_PCM_RING_MASK;
    return s;
}

static void pcm_build_for_clocks(pcm_builder_t *b, unsigned clocks, int sample)
{
    b->phase += clocks * (unsigned)OUT_RATE;
    while (b->phase >= UPD_CLOCK) {
        int s = sample >> 8;
        if (s > 127) s = 127;
        if (s < -128) s = -128;
        if (b->len < b->max)
            b->out[b->len++] = (signed char)s;
        b->phase -= UPD_CLOCK;
    }
}

static void pcm_build_adpcm(pcm_builder_t *b, uint8_t data, int rate)
{
    b->sample += upd_step[b->state][data & 15];
    b->state += upd_state_delta[data & 15];
    if (b->state < 0)
        b->state = 0;
    else if (b->state > 15)
        b->state = 15;
    if (b->sample > 32767)
        b->sample = 32767;
    else if (b->sample < -32768)
        b->sample = -32768;
    pcm_build_for_clocks(b, (unsigned)rate * 4u, b->sample);
}

static uint8_t direct_rd(uint16_t *addr)
{
    uint8_t v;
#if SHINOBI_AUDIO_RUST
    v = audmem[*addr];
#else
    v = z80.memory[*addr];
#endif
    *addr = (uint16_t)(*addr + 1);
    return v;
}

static int direct_pcm_decode_block(pcm_builder_t *b, uint16_t *addr, int *first_header, int depth)
{
    if (depth > 8 || b->len >= b->max - 256u)
        return 1;

    uint8_t header = direct_rd(addr);
    if (!*first_header && header == 0xff)
        return 0;
    if (header == 0x00 && *first_header)
        return 1;

    switch (header & 0xc0) {
    case 0x00:
        b->sample = 0;
        b->state = 0;
        pcm_build_for_clocks(b, 1024u * (unsigned)((header & 0x3f) + 1), 0);
        if (header)
            *first_header = 1;
        break;

    case 0x40: {
        int rate = (header & 0x3f) + 1;
        int nibbles = 256;
        *first_header = 1;
        while (nibbles > 0 && b->len < b->max - 2u) {
            uint8_t data = direct_rd(addr);
            pcm_build_adpcm(b, data >> 4, rate);
            if (--nibbles == 0)
                break;
            pcm_build_adpcm(b, data & 15, rate);
            nibbles--;
        }
        break;
    }

    case 0x80: {
        int rate = (header & 0x3f) + 1;
        int nibbles = (int)direct_rd(addr) + 1;
        *first_header = 1;
        while (nibbles > 0 && b->len < b->max - 2u) {
            uint8_t data = direct_rd(addr);
            pcm_build_adpcm(b, data >> 4, rate);
            if (--nibbles == 0)
                break;
            pcm_build_adpcm(b, data & 15, rate);
            nibbles--;
        }
        break;
    }

    default: {
        int reps = (header & 7) + 1;
        uint16_t repeat_addr = *addr;
        uint16_t after_addr = repeat_addr;
        *first_header = 1;
        for (int i = 0; i < reps && b->len < b->max - 256u; i++) {
            uint16_t a = repeat_addr;
            if (direct_pcm_decode_block(b, &a, first_header, depth + 1))
                return 1;
            if (i == 0)
                after_addr = a;
        }
        *addr = after_addr;
        break;
    }
    }

    return 0;
}

static unsigned direct_pcm_decode_cached(uint8_t ctrl, uint16_t addr, signed char **pcm)
{
    uint32_t key = ((uint32_t)ctrl << 16) | addr;
    for (unsigned i = 0; i < DIRECT_PCM_CACHE_SLOTS; i++) {
        if (direct_pcm_cache[i].key == key && direct_pcm_cache[i].len) {
            *pcm = direct_pcm_cache[i].pcm;
            return direct_pcm_cache[i].len;
        }
    }

    direct_pcm_cache_t *slot = &direct_pcm_cache[direct_pcm_cache_next++ % DIRECT_PCM_CACHE_SLOTS];
    slot->key = key;
    slot->len = 0;

    if (addr < 0x8000 || addr > 0xdfff) {
        *pcm = slot->pcm;
        return 0;
    }

    pcm_builder_t b;
    b.out = slot->pcm;
    b.len = 0;
    b.max = DIRECT_PCM_CACHE_MAX;
    b.phase = 0;
    b.sample = 0;
    b.state = 0;

    int first_header = 0;
    int guard = 4096;
    while (guard-- > 0 && b.len < b.max - 256u) {
        if (direct_pcm_decode_block(&b, &addr, &first_header, 0))
            break;
    }

    slot->len = b.len;
    if (slot->len) {
        unsigned fade_in = slot->len < 32u ? slot->len : 32u;
        unsigned fade_out = slot->len < 64u ? slot->len : 64u;
        for (unsigned i = 0; i < fade_in; i++)
            slot->pcm[i] = (signed char)((int)slot->pcm[i] * (int)i / (int)fade_in);
        for (unsigned i = 0; i < fade_out; i++) {
            unsigned p = slot->len - 1u - i;
            slot->pcm[p] = (signed char)((int)slot->pcm[p] * (int)i / (int)fade_out);
        }
    }
    *pcm = slot->pcm;
    return slot->len;
}

static void direct_pcm_play_cached(uint8_t ctrl, uint16_t addr)
{
    signed char *pcm = 0;
    unsigned len = direct_pcm_decode_cached(ctrl, addr, &pcm);
    if (len < 8u)
        return;
    direct_pcm_reset();
    for (unsigned i = 0; i < len; i++)
        direct_pcm_queue(pcm[i]);
}

#if SHINOBI_UPD_PCM_ENABLED
#define UPD_PCM_RING_SIZE 32768
#define UPD_PCM_RING_MASK (UPD_PCM_RING_SIZE - 1)
#define UPD_CLOCK 640000

enum {
    UPD_PCM_HEADER,
    UPD_PCM_COUNT,
    UPD_PCM_DATA
};

static signed char upd_pcm_ring[UPD_PCM_RING_SIZE];
static unsigned upd_pcm_rd, upd_pcm_wr;
static int upd_pcm_active, upd_pcm_state, upd_pcm_first_header;
static int upd_pcm_rate, upd_pcm_nibbles, upd_pcm_data;
static int upd_pcm_sample, upd_pcm_adpcm_state;
static unsigned upd_pcm_phase;
static int upd_pcm_repeat_remaining, upd_pcm_recording, upd_pcm_replaying;
static uint8_t upd_pcm_repeat_buf[320];
static unsigned upd_pcm_repeat_len;

static unsigned upd_pcm_used(void)
{
    return (upd_pcm_wr - upd_pcm_rd) & UPD_PCM_RING_MASK;
}

static unsigned upd_pcm_free(void)
{
    return UPD_PCM_RING_MASK - upd_pcm_used();
}

static void upd_pcm_queue(signed char s)
{
    unsigned next = (upd_pcm_wr + 1) & UPD_PCM_RING_MASK;
    if (next == upd_pcm_rd)
        return;
    upd_pcm_ring[upd_pcm_wr] = s;
    upd_pcm_wr = next;
}

static int upd_pcm_pop(void)
{
    if (upd_pcm_rd == upd_pcm_wr)
        return 0;
    int s = upd_pcm_ring[upd_pcm_rd];
    upd_pcm_rd = (upd_pcm_rd + 1) & UPD_PCM_RING_MASK;
    return s;
}

static void upd_pcm_request_more(void)
{
    if (upd_pcm_active && upd_pcm_free() > 512)
        upd_nmi_pending = 1;
}

static void upd_pcm_queue_for_clocks(int clocks, int sample)
{
    upd_pcm_phase += (unsigned)clocks * (unsigned)OUT_RATE;
    while (upd_pcm_phase >= UPD_CLOCK) {
        int s = sample >> 8;
        if (s > 127) s = 127;
        if (s < -128) s = -128;
        upd_pcm_queue((signed char)s);
        upd_pcm_phase -= UPD_CLOCK;
    }
}

static void upd_pcm_update_adpcm(int data)
{
    upd_pcm_sample += upd_step[upd_pcm_adpcm_state][data & 15];
    upd_pcm_adpcm_state += upd_state_delta[data & 15];
    if (upd_pcm_adpcm_state < 0)
        upd_pcm_adpcm_state = 0;
    else if (upd_pcm_adpcm_state > 15)
        upd_pcm_adpcm_state = 15;
    if (upd_pcm_sample > 32767)
        upd_pcm_sample = 32767;
    else if (upd_pcm_sample < -32768)
        upd_pcm_sample = -32768;
    upd_pcm_queue_for_clocks(upd_pcm_rate * 4, upd_pcm_sample);
}

static void upd_pcm_feed(uint8_t data);

static void upd_pcm_finish_block(void)
{
    if (upd_pcm_recording) {
        unsigned len = upd_pcm_repeat_len;
        int reps = upd_pcm_repeat_remaining - 1;
        upd_pcm_recording = 0;
        upd_pcm_repeat_remaining = 0;
        while (reps-- > 0) {
            upd_pcm_replaying = 1;
            for (unsigned i = 0; i < len; i++)
                upd_pcm_feed(upd_pcm_repeat_buf[i]);
            upd_pcm_replaying = 0;
        }
    }
}

static void upd_pcm_feed(uint8_t data)
{
    if (upd_pcm_recording && !upd_pcm_replaying && upd_pcm_repeat_len < sizeof upd_pcm_repeat_buf)
        upd_pcm_repeat_buf[upd_pcm_repeat_len++] = data;

    switch (upd_pcm_state) {
    case UPD_PCM_HEADER:
        if (!upd_pcm_first_header && (data & 0xc0) == 0xc0)
            return;
        if ((data & 0xc0) == 0x00) {
            if (data == 0 && upd_pcm_first_header) {
                upd_pcm_active = 0;
                upd_pcm_finish_block();
                return;
            }
            upd_pcm_queue_for_clocks(1024 * ((data & 0x3f) + 1), 0);
            upd_pcm_sample = 0;
            upd_pcm_adpcm_state = 0;
            if (data)
                upd_pcm_first_header = 1;
            upd_pcm_finish_block();
        } else if ((data & 0xc0) == 0x40) {
            upd_pcm_rate = (data & 0x3f) + 1;
            upd_pcm_nibbles = 256;
            upd_pcm_state = UPD_PCM_DATA;
            upd_pcm_first_header = 1;
        } else if ((data & 0xc0) == 0x80) {
            upd_pcm_rate = (data & 0x3f) + 1;
            upd_pcm_state = UPD_PCM_COUNT;
            upd_pcm_first_header = 1;
        } else {
            upd_pcm_repeat_remaining = (data & 7) + 1;
            upd_pcm_recording = !upd_pcm_replaying;
            upd_pcm_repeat_len = 0;
        }
        break;

    case UPD_PCM_COUNT:
        upd_pcm_nibbles = data + 1;
        upd_pcm_state = UPD_PCM_DATA;
        break;

    case UPD_PCM_DATA:
        upd_pcm_data = data;
        upd_pcm_update_adpcm(upd_pcm_data >> 4);
        if (--upd_pcm_nibbles == 0) {
            upd_pcm_state = UPD_PCM_HEADER;
            upd_pcm_finish_block();
            break;
        }
        upd_pcm_update_adpcm(upd_pcm_data & 15);
        if (--upd_pcm_nibbles == 0) {
            upd_pcm_state = UPD_PCM_HEADER;
            upd_pcm_finish_block();
        }
        break;
    }
}

static void upd_pcm_start(void)
{
    upd_pcm_active = 1;
    upd_pcm_state = UPD_PCM_HEADER;
    upd_pcm_first_header = 0;
    upd_pcm_rate = 1;
    upd_pcm_nibbles = 0;
    upd_pcm_data = 0;
    upd_pcm_sample = 0;
    upd_pcm_adpcm_state = 0;
    upd_pcm_phase = 0;
    upd_pcm_repeat_remaining = 0;
    upd_pcm_recording = 0;
    upd_pcm_replaying = 0;
    upd_pcm_repeat_len = 0;
    upd_pcm_request_more();
}

static void upd_pcm_reset(void)
{
    upd_pcm_active = 0;
    upd_pcm_state = UPD_PCM_HEADER;
    upd_pcm_rd = upd_pcm_wr = 0;
    upd_pcm_sample = 0;
    upd_pcm_adpcm_state = 0;
    upd_pcm_phase = 0;
    upd_pcm_repeat_remaining = 0;
    upd_pcm_recording = 0;
    upd_pcm_replaying = 0;
    upd_pcm_repeat_len = 0;
}
#endif

void shinobi_sound_run_cycles(int total);
void shinobi_audio_command(uint8_t v);

static void install_bank(uint8_t data)
{
    if (!shinobi_rom_sample)
        return;
    /* Shadow Dancer's PCM sample ROM (mpr-12715.b4) is 0x40000. shdancer is a
     * System 18 board (2x YM3438 + RF5C68) so this System-16B uPD7759 banked
     * window is not the real sound path -- it stays in-bounds so the loader is
     * safe, but audio is best-effort/quiet on this port. */
    unsigned bankoffs = ((data & 0x08u) >> 3) * 0x20000u;
    bankoffs += (data & 0x07u) * 0x4000u;
    bankoffs %= 0x40000u;
#if SHINOBI_AUDIO_RUST
    for (unsigned i = 0; i < 0x6000u; i++)
        audmem[0x8000u + i] = shinobi_rom_sample[(bankoffs + i) & 0x3ffffu];
#else
    for (unsigned i = 0; i < 0x6000u; i++)
        z80.memory[0x8000u + i] = shinobi_rom_sample[(bankoffs + i) & 0x3ffffu];
#endif
}

static void upd_reset_core(void)
{
    upd_state = UPD_IDLE;
    upd_post_state = UPD_IDLE;
    upd_clocks_left = 0;
    upd_post_clocks = 0;
    upd_drq = 0;
    upd_nmi_pending = 0;
    upd_waiting_for_data = 0;
    upd_servicing_drq = 0;
    upd_nibbles_left = 0;
    upd_repeat_count = 0;
    upd_req_sample = 0;
    upd_last_sample = 0;
    upd_block_header = 0;
    upd_sample_rate = 0;
    upd_first_valid_header = 0;
    upd_offset = 0;
    upd_repeat_offset = 0;
    upd_adpcm_state = 0;
    upd_adpcm_data = 0;
    upd_sample = 0;
    upd_output_active = 0;
    upd_mode_slave = 0;
}

static void upd_set_drq(int v)
{
#if SHINOBI_UPD_ENABLED
    if (v && !upd_drq) {
        upd_nmi_pending = 1;
        if (upd_mode_slave) {
            upd_waiting_for_data = 1;
            if (!upd_servicing_drq) {
                upd_servicing_drq = 1;
                for (int i = 0; i < 8 && upd_waiting_for_data; i++)
                    shinobi_sound_run_cycles(512);
                upd_servicing_drq = 0;
            }
        }
    }
#endif
    upd_drq = v;
}

static void upd_update_adpcm(int data)
{
    upd_sample += upd_step[upd_adpcm_state][data & 15];
    upd_adpcm_state += upd_state_delta[data & 15];
    if (upd_adpcm_state < 0)
        upd_adpcm_state = 0;
    else if (upd_adpcm_state > 15)
        upd_adpcm_state = 15;
    if (upd_sample > 32767)
        upd_sample = 32767;
    else if (upd_sample < -32768)
        upd_sample = -32768;
    upd_output_active = 1;
}

static uint8_t upd_data(void)
{
    if (!upd_mode_slave && shinobi_rom_sample)
        return shinobi_rom_sample[upd_offset & 0x3ffffu];
    return upd_fifo;
}

static void upd_advance_state(void)
{
    switch (upd_state) {
    case UPD_IDLE:
        upd_clocks_left = 4;
        break;
    case UPD_DROP_DRQ:
        upd_set_drq(0);
        upd_clocks_left = upd_post_clocks;
        upd_state = upd_post_state;
        break;
    case UPD_START:
        upd_req_sample = upd_mode_slave ? 0x10 : upd_fifo;
        upd_clocks_left = 70;
        upd_state = UPD_FIRST_REQ;
        break;
    case UPD_FIRST_REQ:
        upd_set_drq(1);
        upd_clocks_left = 44;
        upd_state = UPD_LAST_SAMPLE;
        break;
    case UPD_LAST_SAMPLE:
        upd_last_sample = upd_mode_slave ? upd_fifo : (shinobi_rom_sample ? shinobi_rom_sample[0] : 0);
        upd_set_drq(1);
        upd_clocks_left = 28;
        if (upd_req_sample > upd_last_sample) {
            upd_output_active = 0;
            upd_state = UPD_IDLE;
        } else {
            upd_state = UPD_DUMMY1;
        }
        break;
    case UPD_DUMMY1:
        upd_set_drq(1);
        upd_clocks_left = 32;
        upd_state = UPD_ADDR_MSB;
        break;
    case UPD_ADDR_MSB:
        upd_offset = (unsigned)upd_data() << 9;
        upd_set_drq(1);
        upd_clocks_left = 44;
        upd_state = UPD_ADDR_LSB;
        break;
    case UPD_ADDR_LSB:
        upd_offset |= (unsigned)upd_data() << 1;
        upd_set_drq(1);
        upd_clocks_left = 36;
        upd_state = UPD_DUMMY2;
        break;
    case UPD_DUMMY2:
        upd_offset++;
        upd_first_valid_header = 0;
        upd_set_drq(1);
        upd_clocks_left = 36;
        upd_state = UPD_BLOCK_HEADER;
        break;
    case UPD_BLOCK_HEADER:
        if (upd_repeat_count) {
            upd_repeat_count--;
            upd_offset = upd_repeat_offset;
        }
        upd_block_header = upd_data();
        if (!upd_mode_slave)
            upd_offset++;
        upd_set_drq(1);
        switch (upd_block_header & 0xc0) {
        case 0x00:
            upd_clocks_left = 1024 * ((upd_block_header & 0x3f) + 1);
            upd_state = (upd_block_header == 0 && upd_first_valid_header) ? UPD_IDLE : UPD_BLOCK_HEADER;
            upd_sample = 0;
            upd_adpcm_state = 0;
            upd_output_active = 0;
            break;
        case 0x40:
            upd_sample_rate = (upd_block_header & 0x3f) + 1;
            upd_nibbles_left = 256;
            upd_clocks_left = 36;
            upd_state = UPD_NIBBLE_MSN;
            break;
        case 0x80:
            upd_sample_rate = (upd_block_header & 0x3f) + 1;
            upd_clocks_left = 36;
            upd_state = UPD_NIBBLE_COUNT;
            break;
        default:
            upd_repeat_count = (upd_block_header & 7) + 1;
            upd_repeat_offset = upd_offset;
            upd_clocks_left = 36;
            upd_state = UPD_BLOCK_HEADER;
            break;
        }
        if (upd_block_header)
            upd_first_valid_header = 1;
        break;
    case UPD_NIBBLE_COUNT:
        upd_nibbles_left = upd_data() + 1;
        if (!upd_mode_slave)
            upd_offset++;
        upd_set_drq(1);
        upd_clocks_left = 36;
        upd_state = UPD_NIBBLE_MSN;
        break;
    case UPD_NIBBLE_MSN:
        upd_adpcm_data = upd_data();
        if (!upd_mode_slave)
            upd_offset++;
        upd_update_adpcm(upd_adpcm_data >> 4);
        upd_set_drq(1);
        upd_clocks_left = upd_sample_rate * 4;
        upd_state = (--upd_nibbles_left == 0) ? UPD_BLOCK_HEADER : UPD_NIBBLE_LSN;
        break;
    case UPD_NIBBLE_LSN:
        upd_update_adpcm(upd_adpcm_data & 15);
        upd_clocks_left = upd_sample_rate * 4;
        upd_state = (--upd_nibbles_left == 0) ? UPD_BLOCK_HEADER : UPD_NIBBLE_MSN;
        break;
    }
    if (upd_drq) {
        upd_post_state = upd_state;
        upd_post_clocks = upd_clocks_left - 21;
        if (upd_post_clocks < 1)
            upd_post_clocks = 1;
        upd_state = UPD_DROP_DRQ;
        upd_clocks_left = 21;
    }
}

static void upd_run(int clocks)
{
#if !SHINOBI_UPD_ENABLED
    (void)clocks;
    return;
#else
    if (upd_state == UPD_IDLE || !upd_reset || upd_waiting_for_data)
        return;
    while (clocks > 0 && upd_state != UPD_IDLE) {
        if (upd_clocks_left <= 0)
            upd_advance_state();
        if (upd_waiting_for_data)
            return;
        int n = upd_clocks_left;
        if (n > clocks)
            n = clocks;
        upd_clocks_left -= n;
        clocks -= n;
        if (upd_clocks_left <= 0)
            upd_advance_state();
        if (upd_waiting_for_data)
            return;
    }
#endif
}

static void upd_port_w(uint8_t data)
{
    upd_fifo = data;
    upd_waiting_for_data = 0;
    if (dbg_sample_writes < sizeof dbg_sample_bytes)
        dbg_sample_bytes[dbg_sample_writes] = data;
    dbg_sample_writes++;
#if SHINOBI_UPD_PCM_ENABLED
    if (upd_pcm_active) {
        upd_pcm_feed(data);
        upd_pcm_request_more();
    }
#endif
}

static void upd_control_w(uint8_t data)
{
#if !SHINOBI_UPD_ENABLED
    int old_md = upd_md;
    int new_reset = (data >> 6) & 1;
    int start_edge;
    upd_md = ((~data) >> 7) & 1;
    upd_reset = new_reset;
    start_edge = old_md && !upd_md && upd_reset;
    upd_state = UPD_IDLE;
    upd_sample = 0;
#if SHINOBI_UPD_PCM_ENABLED
    if (!upd_reset) {
        upd_pcm_reset();
    } else if (old_md && !upd_md) {
        upd_pcm_start();
    }
#endif
    install_bank(data);
#if SHINOBI_AUDIO_RUST
    if (start_edge) {
        uint16_t hl = (uint16_t)((A8(4) << 8) | A8(5));
        if (hl < 0x8000 || hl > 0xdfff) {
            uint16_t raw = (uint16_t)audmem[0xf804] | ((uint16_t)audmem[0xf805] << 8);
            hl = (uint16_t)(raw + 0x8000);
        }
        direct_pcm_play_cached(data, hl);
    }
#endif
    return;
#else
    int new_md = ((~data) >> 7) & 1;
    int old_md = upd_md;
    upd_md = new_md;
    if (upd_state == UPD_IDLE && upd_reset) {
        if (old_md && !upd_md) {
            upd_mode_slave = 1;
            upd_state = UPD_START;
            upd_clocks_left = 0;
            upd_output_active = 0;
        } else if (!old_md && upd_md) {
            upd_mode_slave = 0;
        }
    }

    int new_reset = (data >> 6) & 1;
    if (upd_reset && !new_reset) {
        upd_reset_core();
        upd_output_active = 0;
    }
    upd_reset = new_reset;
    install_bank(data);
#endif
}

static int upd_busy_r(void)
{
#if !SHINOBI_UPD_ENABLED
#if SHINOBI_UPD_PCM_ENABLED
    return upd_pcm_active ? 0 : 1;
#else
    return 1;
#endif
#else
    return upd_state == UPD_IDLE;
#endif
}

static long ym_period_a(void)
{
    int ta = (ym_regs[0x10] << 2) | (ym_regs[0x11] & 3);
    /* YM2151 timers are specified in YM clock periods:
     *   A = 64 * (1024 - TA), B = 1024 * (256 - TB).
     * Shinobi's sound Z80 is 5MHz and the YM2151 is 4MHz, so convert to
     * Z80 cycles by multiplying by 5/4: A factor 80, B factor 1280. */
    return (long)(1024 - ta) * 80;
}

static long ym_period_b(void)
{
    int tb = ym_regs[0x12];
    return (long)(256 - tb) * 1280;
}

static void ym_write(int port, uint8_t v)
{
    if (!port) {
        ym_addr = v;
        shinobi_ym2151_write_addr(v);
        return;
    }

    uint8_t r = ym_addr;
    ym_regs[r] = v;
    dbg_ym_writes++;
    if (r == 0x08 && (v & 0x78))
        dbg_keyons++;
    shinobi_ym2151_write_data(v);

    if (r == 0x14) {
        ym_irqen_a = (v >> 2) & 1;
        ym_irqen_b = (v >> 3) & 1;
        if (v & 0x01) { if (!ym_ta_load) ym_ta_count = ym_period_a(); ym_ta_load = 1; }
        else ym_ta_load = 0;
        if (v & 0x02) { if (!ym_tb_load) ym_tb_count = ym_period_b(); ym_tb_load = 1; }
        else ym_tb_load = 0;
        if (v & 0x10) ym_status &= ~0x01;
        if (v & 0x20) ym_status &= ~0x02;
    }
}

static void ym_advance(long cycles)
{
    if (ym_ta_load) {
        ym_ta_count -= cycles;
        while (ym_ta_count <= 0) {
            ym_status |= 0x01;
            ym_ta_count += ym_period_a();
        }
    }
    if (ym_tb_load) {
        ym_tb_count -= cycles;
        while (ym_tb_count <= 0) {
            ym_status |= 0x02;
            ym_tb_count += ym_period_b();
        }
    }
}

static int ym_irq_active(void)
{
    return ((ym_status & 0x01) && ym_irqen_a) || ((ym_status & 0x02) && ym_irqen_b);
}

#if SHINOBI_AUDIO_RUST
static void aud_push_pc(uint16_t pc)
{
    uint16_t sp = (uint16_t)(A16(A_SP) - 2);
    audmem[sp] = (uint8_t)pc;
    audmem[(uint16_t)(sp + 1)] = (uint8_t)(pc >> 8);
    ASET16(A_SP, sp);
}

static int aud_inject38(void)
{
    if (!A8(A_IFF1))
        return 0;
    aud_push_pc(A16(A_PC));
    A8(A_IFF1) = 0;
    A8(A_IFF2) = 0;
    A8(A_HALTED) = 0;
    A8(A_R) = (A8(A_R) & 0x80) | ((A8(A_R) + 1) & 0x7f);
    ASET16(A_PC, 0x0038);
    return 1;
}

static void aud_inject_nmi(void)
{
    aud_push_pc(A16(A_PC));
    A8(A_IFF2) = A8(A_IFF1);
    A8(A_IFF1) = 0;
    A8(A_HALTED) = 0;
    A8(A_R) = (A8(A_R) & 0x80) | ((A8(A_R) + 1) & 0x7f);
    ASET16(A_PC, 0x0066);
}
#endif

static void sound_reset(void)
{
#if !SHINOBI_AUDIO_RUST
    memset(&z80, 0, sizeof z80);
    memcpy(z80.memory, shinobi_rom_sound, 0x8000);
#endif
#if SHINOBI_AUDIO_RUST
    memset(audmem, 0, sizeof audmem);
    memcpy(audmem, shinobi_rom_sound, 0x8000);
    memset(aud_state, 0, sizeof aud_state);
    A8(6) = 0xff;
    A8(7) = 0xff;
    ASET16(A_SP, 0xffff);
    ASET16(A_PC, 0x0000);
    ASET32(44, (unsigned long)audmem);
#endif
#if !SHINOBI_AUDIO_RUST
    z80.opcodes = 0;
    z80.opcodes_len = 0;
#endif
    install_bank(0);
#if !SHINOBI_AUDIO_RUST
    Z80Reset(&z80.state);
#endif
    memset(ym_regs, 0, sizeof ym_regs);
    latch = response = ym_addr = ym_status = 0;
    latch_irq = latch_pending = 0;
    ym_irqen_a = ym_irqen_b = ym_ta_load = ym_tb_load = 0;
    ym_irq_asserted = 0;
    ym_ta_count = ym_tb_count = 0;
    dbg_ym_writes = dbg_keyons = dbg_sample_writes = 0;
    dbg_commands = dbg_high_commands = dbg_pcm_nonzero = 0;
    memset(dbg_sample_bytes, 0, sizeof dbg_sample_bytes);
    dbg_last_command = 0;
    last_accepted_command = 0;
    command_guard_samples = 0;
    z80_cycle_acc = 0;
    z80_rate_acc = 0;
    direct_pcm_reset();
    upd_reset_core();
    upd_md = 1;
    upd_reset = 0;
    upd_fifo = 0;
#if SHINOBI_UPD_PCM_ENABLED
    upd_pcm_reset();
#endif
    shinobi_ym2151_reset();
    audio_ready = shinobi_rom_sound != 0;
}

void shinobi_sound_init(void)
{
    if (!shinobi_rom_sound)
        return;
    sound_reset();
}

void shinobi_sound_run_cycles(int total)
{
    if (!audio_ready)
        return;
    while (total > 0) {
        int n = total > 512 ? 512 : total;
#if SHINOBI_AUDIO_RUST
        if (upd_nmi_pending) {
            upd_nmi_pending = 0;
            aud_inject_nmi();
        }
        /* The sound command latch is edge-IRQ driven: MAME asserts the sound
         * Z80 /INT only on a command write. The YM2151 timer-A is polled by the
         * sound program's own main loop (0x0b09 spins on IN A,(0x01) bit0 and
         * reloads reg 0x14), it is NOT IRQ-driven. Driving RST 38h from the YM
         * timer made the handler at 0x006e re-read the command latch every
         * timer period; the latch holds the last command, so the same sample
         * was re-queued every period, flooding the 8-slot sample queue at 0xf808
         * -> the last SFX (often an explosion) replayed continuously and new
         * SFX were dropped (queue-full path at 0x008b). Inject RST 38h on the
         * command latch only. ym_status is still maintained by ym_advance so the
         * polled 0x0b09 loop keeps ticking. */
        if (latch_irq) {
            aud_inject38();
        }
        ASET32(A_CYCLES, 0);
        ASET32(A_BUDGET, n);
        run(aud_state);
        ym_advance(n);
        total -= n;
#else
        if (upd_nmi_pending) {
            upd_nmi_pending = 0;
            Z80NonMaskableInterrupt(&z80.state, &z80);
        }
        /* Command latch is edge-IRQ only; YM timer-A is polled (see above). */
        if (latch_irq) {
            Z80Interrupt(&z80.state, 0xff, &z80);
        }
        int did = Z80Emulate(&z80.state, n, &z80);
        if (did <= 0) did = n;
        ym_advance(did);
        total -= did;
#endif
    }
}

void shinobi_sound_render(signed char *out, int n)
{
    for (int i = 0; i < n; i++) {
        z80_rate_acc += Z80_CLOCK;
        z80_cycle_acc += (int)(z80_rate_acc / OUT_RATE);
        z80_rate_acc %= OUT_RATE;
        if (z80_cycle_acc >= Z80_AUDIO_QUANTUM) {
            shinobi_sound_run_cycles(z80_cycle_acc);
            z80_cycle_acc = 0;
        }
#if SHINOBI_UPD_PCM_ENABLED
        upd_pcm_request_more();
#endif
#if SHINOBI_UPD_ENABLED
        upd_run(640000 / OUT_RATE);
#endif
        int fm = shinobi_ym2151_sample();
#if SHINOBI_UPD_ENABLED
        int s = (fm >> 7) + (upd_output_active ? (upd_sample >> 8) : 0);
#elif SHINOBI_UPD_PCM_ENABLED
        int s = (fm >> 7) + upd_pcm_pop();
#else
        int s = (fm >> 7) + (direct_pcm_pop() * DIRECT_PCM_MIX_NUM) / DIRECT_PCM_MIX_DEN;
#endif
        /* Soft-clip the overdriven FM sum (fm>>7 swings +-255) down to the
         * +-127 8-bit Paula range with a smooth cubic saturator y = x - x^3/3
         * (x = s/256 clamped to [-1,1], out = y*190). The old 10:1 linear fold
         * flat-topped loud transients into a buzzy "crackle" on percussive SFX
         * (notably the kid-rescue sound); the cubic has no flat plateau, so it
         * removes that crackle while keeping ~80% of the loudness. s^3*190
         * (<= ~3.2e9) overflows int32, hence the long long. */
        if (s > 256) s = 256; else if (s < -256) s = -256;
        {   long long s3 = (long long)s * s * s;
            s = (190 * s) / 256 - (int)((s3 * 190) / 50331648);
        }
        if (s > 127) s = 127;
        if (s < -128) s = -128;
        if (s)
            dbg_pcm_nonzero++;
        out[i] = (signed char)s;
        if (command_guard_samples > 0)
            command_guard_samples--;
    }
}

void shinobi_audio_command(uint8_t v)
{
    if (v == 0xff)
        return;
    last_accepted_command = v;
    command_guard_samples = 0;
    latch = v;
    latch_pending = 1;
    latch_irq = 1;
    dbg_last_command = v;
    dbg_commands++;
    if (v & 0x80)
        dbg_high_commands++;
}

static uint8_t latch_pop(void)
{
    latch_irq = 0;
    return latch;
}

void shinobi_audio_pulse(uint8_t v)
{
    (void)v;
}

uint8_t shinobi_audio_response(void)
{
    return response;
}

#if SHINOBI_AUDIO_RUST
uint8_t shinobi_aud_latch(void)
{
    return latch_pop();
}

uint8_t shinobi_aud_in(uint8_t port)
{
    port &= 0xff;
    if ((port & 0xc0) == 0x00)
        return shinobi_ym2151_read_status() | ym_status;
    if ((port & 0xc0) == 0xc0)
        return latch_pop();
    if ((port & 0xc0) == 0x80)
        return (uint8_t)(upd_busy_r() << 7);
    return 0xff;
}

void shinobi_aud_out(uint8_t port, uint8_t x)
{
    port &= 0xff;
    if ((port & 0xc0) == 0x00) {
        ym_write(port & 1, x);
        return;
    }
    if ((port & 0xc0) == 0x40) {
        upd_control_w(x);
        return;
    }
    if ((port & 0xc0) == 0x80) {
        upd_port_w(x);
        return;
    }
    if ((port & 0xc0) == 0xc0) {
        response = x;
        return;
    }
}
#endif

#if !SHINOBI_AUDIO_RUST
unsigned char machine_rd(MY_LITTLE_Z80 *z, unsigned int addr)
{
    (void)z;
    addr &= 0xffff;
    if (addr == 0xe800) {
        return latch_pop();
    }
    if (addr >= 0xf800)
        return z80.memory[addr];
    return z80.memory[addr];
}

void machine_wr(MY_LITTLE_Z80 *z, unsigned int addr, unsigned char val)
{
    (void)z;
    addr &= 0xffff;
    if (addr >= 0xf800) {
        z80.memory[addr] = val;
        return;
    }
}

unsigned char in_impl(MY_LITTLE_Z80 *z, int port)
{
    (void)z;
    port &= 0xff;
    if ((port & 0xc0) == 0x00)
        return shinobi_ym2151_read_status() | ym_status;
    if ((port & 0xc0) == 0xc0) {
        return latch_pop();
    }
    if ((port & 0xc0) == 0x80)
        return (uint8_t)(upd_busy_r() << 7);
    return 0xff;
}

void out_impl(MY_LITTLE_Z80 *z, int port, unsigned char x)
{
    (void)z;
    port &= 0xff;
    if ((port & 0xc0) == 0x00) {
        ym_write(port & 1, x);
        return;
    }
    if ((port & 0xc0) == 0x40) {
        upd_control_w(x);
        return;
    }
    if ((port & 0xc0) == 0x80) {
        upd_port_w(x);
        return;
    }
    if ((port & 0xc0) == 0xc0) {
        response = x;
        return;
    }
}
#endif

unsigned shinobi_sound_dbg_ym_writes(void) { return dbg_ym_writes; }
unsigned shinobi_sound_dbg_keyons(void) { return dbg_keyons; }
unsigned shinobi_sound_dbg_sample_writes(void) { return dbg_sample_writes; }
unsigned shinobi_sound_dbg_commands(void) { return dbg_commands; }
unsigned shinobi_sound_dbg_high_commands(void) { return dbg_high_commands; }
unsigned shinobi_sound_dbg_last_command(void) { return dbg_last_command; }
unsigned shinobi_sound_dbg_pcm_nonzero(void) { return dbg_pcm_nonzero; }
unsigned shinobi_sound_dbg_sample_byte(unsigned i) { return i < sizeof dbg_sample_bytes ? dbg_sample_bytes[i] : 0; }
