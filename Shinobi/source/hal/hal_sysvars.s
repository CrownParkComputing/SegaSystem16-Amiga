; src/hal/hal_sysvars.s
; ============================================================
;  SysBase + SysVars + ExecBase -- the few global pointers the
;  C runtime needs to call Exec library functions.
; ============================================================
;
; The libnix <proto/exec.h> references `_SysBase` for the
; current task's Exec library base. We provide it here so
; the C side can use AllocMem and friends without dragging
; in the full libnix startup.
;
; amiga_main does:
;     move.l  a6, _SysBase
; at startup, which is then used by every C function that
; calls into Exec.
;
; Other libnix globals (_OSERR, _stdout, _stdin, etc.) are
; not needed for this project's use of libnix headers.
; ============================================================

        XDEF    _SysBase

        SECTION data,DATA

_SysBase:
        dc.l    0       ; populated by amiga_main at startup
