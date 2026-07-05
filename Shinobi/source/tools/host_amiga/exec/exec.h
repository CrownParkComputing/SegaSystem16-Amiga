/* Host shim for <exec/exec.h>. */
#ifndef HOST_SHIM_EXEC_EXEC_H
#define HOST_SHIM_EXEC_EXEC_H

#include <exec/types.h>
#include <exec/memory.h>

#ifdef __cplusplus
extern "C" {
#endif
void *OpenLibrary(const char *libName, unsigned long version);
void  CloseLibrary(void *base);
void  Forbid(void);
void  Permit(void);
#ifdef __cplusplus
}
#endif

#endif
