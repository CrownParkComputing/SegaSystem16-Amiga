#ifndef DRIVER_H
#define DRIVER_H
#include <stdio.h>
typedef unsigned char  UINT8;  typedef unsigned short UINT16; typedef unsigned int UINT32;
typedef signed char    INT8;   typedef signed short   INT16;  typedef signed int   INT32;
#define INLINE static __inline
static __inline void logerror(const char *f, ...){ (void)f; }
double timer_get_time(void);
#endif
