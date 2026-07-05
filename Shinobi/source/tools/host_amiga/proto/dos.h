/* Host shim for <proto/dos.h>. The DOS file calls are only reached by
 * shinobi_interp.c's save-state paths, which the render harness never invokes;
 * the host stubs are inert. */
#ifndef HOST_SHIM_PROTO_DOS_H
#define HOST_SHIM_PROTO_DOS_H

#include <exec/types.h>
#include <dos/dos.h>

BPTR Open(CONST_STRPTR name, long accessMode);
LONG Close(BPTR file);
LONG Read(BPTR file, void *buffer, long length);
LONG Write(BPTR file, void *buffer, long length);
BPTR CreateDir(CONST_STRPTR name);
void UnLock(BPTR lock);

#endif
