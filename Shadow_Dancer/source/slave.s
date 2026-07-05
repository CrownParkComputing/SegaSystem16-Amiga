; src/slave.s
; ============================================================
;  pacland_amiga.slave  --  WHDLoad slave skeleton
; ============================================================
;
; This is a working STUB slave. It loads, calls our amiga_main,
; waits for left mouse button to exit, and returns control to
; WHDLoad. Nothing else.
;
; Entry contract (WHDLoad):
;   a0 = pointer to the slv_ struct (not used in our stub)
;   a6 = ExecBase
;   sp = WHDLoad stack (safe to clobber after we save it)
;
; For CLI launching (Amiberry "Run Program"), we use _start in
; src/amiga/amiga.s instead -- this file is WHDLoad-only.
;
; To flesh it out for a real release:
;   1. add WHDLoad slave-header magic (the standard _slv_ struct)
;      using WHDLoad's whdload.i
;   2. add patchable-entry handling for Kick 1.3 / 2.04 / 3.0+
;   3. add the standard WHDLoad exception/intercept macros
;   4. tag with the proper WHDLoad-Author / -Game tag
;
; Assembled with vasm motorola syntax.
; ============================================================

        XDEF    _start
        XREF    amiga_main                ; defined in src/amiga/amiga.s

        SECTION code,CODE

; --- Cold entry point WHDLoad jumps to ---
_start:
        movem.l d0-d7/a0-a5,-(sp)
        move.l  a6,-(sp)               ; push ExecBase
        bsr     amiga_main
        addq.w  #4,sp
        movem.l (sp)+,d0-d7/a0-a5
        rts

        END
