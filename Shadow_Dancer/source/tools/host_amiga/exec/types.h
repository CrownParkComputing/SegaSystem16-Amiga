/* Host shim for <exec/types.h> -- just enough for shinobi_interp.c and
 * shinobi_hwrender.c to compile with a native (Linux) C compiler. */
#ifndef HOST_SHIM_EXEC_TYPES_H
#define HOST_SHIM_EXEC_TYPES_H

#include <stdint.h>

typedef int32_t  LONG;
typedef uint32_t ULONG;
typedef int16_t  WORD;
typedef uint16_t UWORD;
typedef int8_t   BYTE;
typedef uint8_t  UBYTE;
typedef int      BOOL;
typedef void    *APTR;
typedef long     BPTR;
typedef char       *STRPTR;
typedef const char *CONST_STRPTR;

#ifndef CONST
#define CONST const
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

struct Library;   /* opaque */

#endif
