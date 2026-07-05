/* shinobi_hostsnd_stubs.c -- host (Linux native cc) implementations of the
 * AmigaOS APIs referenced by shinobi_interp.c and shinobi_hwrender.c when built
 * with -DSHINOBI_RTG. Unlike the render-only harness, the REAL sound engine
 * (shinobi_sound.c + z80.c + shinobi_ym2151.cpp + ymfm) IS linked here, so we do
 * NOT stub shinobi_audio_command / shinobi_audio_pulse / shinobi_audio_response
 * (those are the real ones under test). Only Paula-poking and OS glue are stubbed.
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
    return &s_lib_dummy;
}
void CloseLibrary(void *base) { (void)base; }
void Forbid(void) {}
void Permit(void) {}

/* ---- graphics (only referenced by the compiled-out non-RTG path) ---- */
void LoadView(void *view) { (void)view; }
void WaitTOF(void) {}

/* ---- DOS (save-state paths only; never exercised by this harness) ---- */
BPTR Open(CONST_STRPTR name, long accessMode) { (void)name; (void)accessMode; return 0; }
LONG Close(BPTR file) { (void)file; return 0; }
LONG Read(BPTR file, void *buffer, long length) { (void)file; (void)buffer; (void)length; return -1; }
LONG Write(BPTR file, void *buffer, long length) { (void)file; (void)buffer; (void)length; return -1; }
BPTR CreateDir(CONST_STRPTR name) { (void)name; return 0; }
void UnLock(BPTR lock) { (void)lock; }

struct DosLibrary *DOSBase = 0;
