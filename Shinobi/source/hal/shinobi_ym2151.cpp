/* C wrapper around ymfm's YM2151/OPM core for Shinobi's System 16 sound path. */
#include <stdint.h>
#include <stddef.h>
#include <new>
#include <exec/exec.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <ymfm.h>
#include <ymfm_opm.h>

#define SH_OUT_RATE     22050
#define SH_YM2151_CLOCK 4000000

void *operator new(size_t n)
{
    void *p = AllocMem((unsigned long)n, MEMF_FAST | MEMF_CLEAR);
    if (!p) p = AllocMem((unsigned long)n, MEMF_PUBLIC | MEMF_CLEAR);
    return p;
}

void *operator new[](size_t n)
{
    void *p = AllocMem((unsigned long)n, MEMF_FAST | MEMF_CLEAR);
    if (!p) p = AllocMem((unsigned long)n, MEMF_PUBLIC | MEMF_CLEAR);
    return p;
}

void operator delete(void *) noexcept { }
void operator delete[](void *) noexcept { }
void operator delete(void *, size_t) noexcept { }
void operator delete[](void *, size_t) noexcept { }

namespace {

struct sh_ym_intf : public ymfm::ymfm_interface
{
    void ymfm_set_timer(uint32_t, int32_t) override { }
    void ymfm_update_irq(bool) override { }
};

alignas(sh_ym_intf) static unsigned char intf_store[sizeof(sh_ym_intf)];
static sh_ym_intf *intf_ptr = 0;
alignas(ymfm::ym2151) static unsigned char chip_store[sizeof(ymfm::ym2151)];
static ymfm::ym2151 *chip_ptr = 0;

static sh_ym_intf &intf()
{
    if (!intf_ptr)
        intf_ptr = new (intf_store) sh_ym_intf();
    return *intf_ptr;
}

static ymfm::ym2151 *chip()
{
    if (!chip_ptr)
        chip_ptr = new (chip_store) ymfm::ym2151(intf());
    return chip_ptr;
}

}

extern "C" {

void shinobi_ym2151_reset(void)
{
    chip()->reset();
}

void shinobi_ym2151_write_addr(uint8_t v)
{
    chip()->write_address(v);
}

void shinobi_ym2151_write_data(uint8_t v)
{
    chip()->write_data(v);
}

uint8_t shinobi_ym2151_read_status(void)
{
    return 0;
}

int shinobi_ym2151_sample(void)
{
    static uint32_t step = 0, frac = 0;
    /* Decimating low-pass state (Q8 fixed point). The YM2151 runs at
     * clock/64 = 62500Hz but we output at 22050Hz. The old code averaged only
     * the last 2 of the ~3 generated native samples, which is no anti-alias
     * filter at all (first null at 31250Hz): the YM2151 NOISE generator (used
     * for Shinobi's percussion) is broadband to ~31kHz, so everything above the
     * 11025Hz output Nyquist folded straight back as a harsh near-Nyquist buzz
     * ("crash noise"). Run a proper 2-pole low-pass over EVERY native sample and
     * decimate by sampling its output, so out-of-band content is removed before
     * it can alias. Integer fixed point keeps it fast on the FPU-less 68020. */
    static long lp1 = 0, lp2 = 0;
    if (step == 0)
        step = (uint32_t)(((uint64_t)(SH_YM2151_CLOCK / 64) << 16) / (uint32_t)SH_OUT_RATE);
    frac += step;
    int count = (int)(frac >> 16);
    frac &= 0xffff;
    if (count < 1) count = 1;

    /* alpha ~= 0.34 (22/64): two cascaded 1-pole sections give ~7kHz/12dB-oct
     * at 62500Hz, ~-14dB at the 11025Hz Nyquist and steeper above, which tames
     * the aliased noise while preserving the FM voices' <7kHz musical band. */
    ymfm::ym2151::output_data out;
    for (int i = 0; i < count; i++) {
        chip()->generate(&out, 1);
        long mono = ((long)out.data[0] + (long)out.data[1]) >> 1;   /* +-32768 */
        lp1 += (((mono << 8) - lp1) * 22) >> 6;
        lp2 += ((lp1 - lp2) * 22) >> 6;
    }
    int s = (int)(lp2 >> 8);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return s;
}

}
