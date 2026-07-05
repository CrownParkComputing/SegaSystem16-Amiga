/* Host shim for <exec/memory.h>. All MEMF_* are 0 (per the harness spec); the
 * host AllocMem always returns zeroed memory (matches every MEMF_CLEAR caller). */
#ifndef HOST_SHIM_EXEC_MEMORY_H
#define HOST_SHIM_EXEC_MEMORY_H

#include <exec/types.h>

#define MEMF_ANY     0
#define MEMF_PUBLIC  0
#define MEMF_CHIP    0
#define MEMF_FAST    0
#define MEMF_LOCAL   0
#define MEMF_CLEAR   0
#define MEMF_REVERSE 0

#ifdef __cplusplus
extern "C" {
#endif
void *AllocMem(unsigned long byteSize, unsigned long requirements);
void  FreeMem(void *memoryBlock, unsigned long byteSize);
#ifdef __cplusplus
}
#endif

#endif
