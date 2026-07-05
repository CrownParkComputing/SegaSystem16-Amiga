/* Host shim for <proto/graphics.h>. Only referenced by the non-RTG disp_open()
 * path, which is compiled out under -DSHINOBI_RTG; stubs exist for safety. */
#ifndef HOST_SHIM_PROTO_GRAPHICS_H
#define HOST_SHIM_PROTO_GRAPHICS_H

#include <exec/types.h>

void LoadView(void *view);
void WaitTOF(void);

#endif
