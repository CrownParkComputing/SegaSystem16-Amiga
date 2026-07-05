/* src/hal/shinobi_hwmain.c -- native Shinobi (Sega System 16B) driver entry
 * points (hal_game_init / hal_game_frame, called by src/amiga/amiga.s).
 *
 * First-light: the 68000->020 DYNAMIC BINARY TRANSLATOR (shinobi_dyntrans_amiga.c)
 * runs the main CPU as native rebased 020 code.  The AGA renderer and the Z80
 * sound core are STUBBED (a separate agent owns the renderer via the weak
 * shinobi_render hook; sound just captures the latch byte).  Inputs are held at
 * "nothing pressed", matching the proven no-input host reference. */
#include <stdint.h>
#include <exec/types.h>
#include <proto/exec.h>

extern int  shinobi_dyntrans_init(void);
extern void shinobi_dyntrans_frame(void);
extern void shinobi_dyntrans_set_inputs(uint8_t,uint8_t,uint8_t,uint8_t,uint8_t);
extern void shinobi_hw_open(void);     /* AGA display takeover (shinobi_hwrender.c) */

static int g_ok;
static volatile int g_quit;

/* Run the per-frame translator loop ENTIRELY in supervisor.
 *
 * The dynamic translator runs the guest's OWN privileged instructions
 * (move-to/from-SR, movec CACR) plus the dispatch trampoline's; in USER mode
 * those trap.  amiga.s launches us as a plain CLI process (user mode).  A
 * per-FRAME Exec Supervisor() round-trip proved fragile here: the guest leaves
 * the live 020 SR at its own (Sega) IPL each frame and the supervised call would
 * not cleanly drop back to user mode, so frame 2 (and thus the software IRQ4 that
 * releases the boot's vblank wait-spin) never ran.  Instead we enter supervisor
 * ONCE and run the whole frame loop here -- the same sustained-supervisor model
 * the other native cores use via their _rt.s takeover.  shinobi_dyntrans_frame
 * masks/restores the Amiga interrupt mask around each guest frame itself. */
static void shinobi_super_loop(void)
{
    int hold = 0;
    while (!g_quit) {
        shinobi_dyntrans_frame();
        /* CIA-A PRA $bfe001 bit6 = port-0 fire = LEFT MOUSE (active low).
         * Quit only on a deliberate ~3s hold (interrupts are restored between
         * frames, so this read is valid). */
        if ((*(volatile unsigned char *)0xBFE001u & 0x40) == 0) {
            if (++hold >= 180) g_quit = 1;
        } else {
            hold = 0;
        }
    }
}

void hal_game_init(void)
{
    g_ok = shinobi_dyntrans_init();
    /* no-input reference: P1/P2/service all released (active-low 0xFF), DSWs FF */
    shinobi_dyntrans_set_inputs(0xff,0xff,0xff,0xff,0xff);
    /* Take over the AGA display in USER mode (OpenLibrary/LoadView/Forbid run here,
     * once, before the per-frame loop enters supervisor).  After this the per-frame
     * shinobi_render() only touches custom registers, which is safe in supervisor. */
    if (g_ok) shinobi_hw_open();
}

void hal_game_frame(void)
{
    if (!g_ok || g_quit) return;       /* alloc failed (needs ~16MB fast RAM)       */
    Supervisor((APTR)shinobi_super_loop);
}
