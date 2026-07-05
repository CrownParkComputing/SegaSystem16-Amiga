/* Host shim for <dos/dos.h>. */
#ifndef HOST_SHIM_DOS_DOS_H
#define HOST_SHIM_DOS_DOS_H

#include <exec/types.h>

#define MODE_OLDFILE   1005
#define MODE_NEWFILE   1006
#define MODE_READWRITE 1004

struct DosLibrary { int _dummy; };

#endif
