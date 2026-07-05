/* shdancer_host_stubs.c -- host (Linux native cc) implementations of the handful
 * of AmigaOS APIs that shinobi_interp.c and shinobi_hwrender.c reference when
 * built with -DSHINOBI_RTG. Signatures come straight from the shim headers in
 * tools/host_amiga so there are no conflicting-type errors.
 *
 * - AllocMem/FreeMem  -> calloc/free (always zeroed: every caller passes
 *                        MEMF_CLEAR, and the guest RAM model assumes zero-init).
 * - OpenLibrary/etc.  -> harmless non-NULL / no-op stubs.
 * - Open/Read/Write.. -> inert; only interp.c's unused save-state path calls them.
 * - shinobi_audio_*   -> no-op; the sound engine (shinobi_sound.c/ymfm) is not
 *                        linked into the render harness.
 */
#include <stdint.h>
#include <stdlib.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <graphics/gfxbase.h>

/* ---- memory ---- */
void *AllocMem(unsigned long byteSize, unsigned long requirements)
{
    (void)requirements;
    return calloc(1, byteSize ? byteSize : 1);
}
void FreeMem(void *memoryBlock, unsigned long byteSize)
{
    (void)byteSize;
    free(memoryBlock);
}

/* ---- exec library / task ---- */
static int s_lib_dummy;
void *OpenLibrary(const char *libName, unsigned long version)
{
    (void)libName; (void)version;
    return &s_lib_dummy;   /* non-NULL so ensure_dos()/disp_open() proceed */
}
void CloseLibrary(void *base) { (void)base; }
void Forbid(void) {}
void Permit(void) {}

/* ---- graphics (only referenced by the compiled-out non-RTG path) ---- */
void LoadView(void *view) { (void)view; }
void WaitTOF(void) {}

/* ---- DOS (save-state paths only; never exercised by the render harness) ---- */
BPTR Open(CONST_STRPTR name, long accessMode) { (void)name; (void)accessMode; return 0; }
LONG Close(BPTR file) { (void)file; return 0; }
LONG Read(BPTR file, void *buffer, long length) { (void)file; (void)buffer; (void)length; return -1; }
LONG Write(BPTR file, void *buffer, long length) { (void)file; (void)buffer; (void)length; return -1; }
BPTR CreateDir(CONST_STRPTR name) { (void)name; return 0; }
void UnLock(BPTR lock) { (void)lock; }

/* DOSBase normally lives in the port's shinobi_assets.c (not linked on host). */
struct DosLibrary *DOSBase = 0;

/* ---- audio (shinobi_sound.c not linked into the render harness) ---- */
void    shinobi_audio_command(uint8_t v)  { (void)v; }
void    shinobi_audio_pulse(uint8_t v)    { (void)v; }
uint8_t shinobi_audio_response(void)      { return 0; }
