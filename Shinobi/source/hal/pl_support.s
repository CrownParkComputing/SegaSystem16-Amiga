; src/hal/pl_support.s -- minimal libc primitives for the freestanding Pac-Land
; Amiga build (vlink can't consume the toolchain libc.a hunk archive). Plain
; m68k, stack-argument calling convention (gcc default, -noixemul). 32-bit
; counted loops so the plane-clear (80 KB) works (dbra is 16-bit only).

        XDEF    _memset
        XDEF    _memcpy
        XDEF    _memmove
        XDEF    _setjmp
        XDEF    _longjmp

        SECTION code,CODE

; void *memset(void *dst, int c, size_t n)
_memset:
        move.l  4(sp),a0
        move.b  11(sp),d1            ; low byte of int c
        move.l  12(sp),d0            ; n
        move.l  a0,a1                ; keep dst for return
        tst.l   d0
        beq.s   .ms_done
.ms_lp: move.b  d1,(a0)+
        subq.l  #1,d0
        bne.s   .ms_lp
.ms_done:
        move.l  a1,d0               ; return dst
        rts

; void *memcpy(void *dst, const void *src, size_t n)
_memcpy:
        move.l  4(sp),a1            ; dst
        move.l  8(sp),a0            ; src
        move.l  12(sp),d0           ; n
        move.l  a1,d1               ; keep dst for return
        tst.l   d0
        beq.s   .mc_done
.mc_lp: move.b  (a0)+,(a1)+
        subq.l  #1,d0
        bne.s   .mc_lp
.mc_done:
        move.l  d1,d0
        rts

; void *memmove(void *dst, const void *src, size_t n) -- overlap safe
_memmove:
        move.l  4(sp),a1            ; dst
        move.l  8(sp),a0            ; src
        move.l  12(sp),d0           ; n
        move.l  a1,d1               ; return dst
        tst.l   d0
        beq.s   .mm_done
        cmp.l   a0,a1               ; dst <= src -> forward copy
        bls.s   .mm_fwd
        ; backward copy (dst > src)
        add.l   d0,a0
        add.l   d0,a1
.mm_blp:
        move.b  -(a0),-(a1)
        subq.l  #1,d0
        bne.s   .mm_blp
        bra.s   .mm_done
.mm_fwd:
        move.b  (a0)+,(a1)+
        subq.l  #1,d0
        bne.s   .mm_fwd
.mm_done:
        move.l  d1,d0
        rts

; int setjmp(jmp_buf env) -- saves pc/sp/d2-d7/a2-a6 (52 bytes; toolchain
; jmp_buf is larger). Returns 0 directly, retval via longjmp.
_setjmp:
        move.l  4(sp),a0
        move.l  (sp),(a0)           ; return PC
        lea     4(sp),a1
        move.l  a1,4(a0)            ; SP as it will be after our rts
        movem.l d2-d7/a2-a6,8(a0)
        moveq   #0,d0
        rts

; void longjmp(jmp_buf env, int val)
_longjmp:
        move.l  4(sp),a0
        move.l  8(sp),d0
        bne.s   .lj_nz
        moveq   #1,d0               ; longjmp(.,0) must return 1
.lj_nz:
        movem.l 8(a0),d2-d7/a2-a6
        move.l  4(a0),sp
        move.l  (a0),a1
        jmp     (a1)

        END
