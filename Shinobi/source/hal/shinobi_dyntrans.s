; src/hal/shinobi_dyntrans.s -- asm dispatch trampoline for the Shinobi
; 68000->68EC020 dynamic binary translator.  See src/hal/shinobi_dyntrans_amiga.c.
;
; The guest register file lives in memory (_g_dregs/_g_sp/_g_pc/_g_sr).  A
; translated block runs with the guest registers LIVE in the real 020 registers
; (d0-d7/a0-a6, a7=guest stack); on a control-flow boundary the block's emitted
; terminator stub stores the next GUEST pc into _g_pc and jumps to _exit_thunk,
; which spills the guest registers back to memory and returns to the C dispatcher.
;
; Privileged instructions (movec CACR, move SR) require SUPERVISOR mode -- the
; bare-metal takeover (slave.s/amiga.s) runs supervisor, so this is satisfied.
;
                XDEF    _block_enter
                XDEF    _exit_thunk
                XDEF    _shinobi_super_reset

                XREF    _g_dregs            ; d0-d7,a0-a6  (15 longs, movem order)
                XREF    _g_sp               ; guest a7  (= base + guest SP)
                XREF    _g_sr               ; virtual guest SR system byte (word)
                XREF    _g_ccr              ; CCR carried block-to-block (word)
                ; _g_pc is written directly by emitted block-terminator stubs.
                XREF    _g_host_sp          ; saved host stack pointer
                XREF    _g_enter_target     ; host code ptr to enter

                SECTION text,CODE

; void block_enter(void *host_block)
;   enters a translated block; returns (via _exit_thunk's rts) when the block
;   hits a control-flow boundary, with the guest register file updated in memory.
_block_enter:
                move.l  4(sp),a0            ; a0 = host_block (param, pre-push)
                movem.l d2-d7/a2-a6,-(sp)   ; preserve C callee-saved registers
                move.l  a0,(_g_enter_target)
                move.l  sp,(_g_host_sp)     ; host sp -> points at saved C regs
                move.w  (_g_ccr),ccr        ; USER-MODE: restore carried CCR only (move-to-CCR is
                                            ; user-legal; system byte stays virtual in g_sr)
                movem.l (_g_dregs),d0-d7/a0-a6
                move.l  (_g_sp),a7          ; switch to guest stack
                jmp     ([_g_enter_target.l]); enter block (020 memory-indirect)

; _exit_thunk -- jumped to by every block terminator stub.  Guest regs are live;
; _g_pc already holds the next guest pc.  Spill regs, restore C state, return.
; NOTE: SR is NOT saved here.  By the time exit_thunk runs the terminator stub
; has already executed a flag-clobbering `move.l #pc,(g_pc)`, so the live SR's
; CCR is garbage.  Each terminator stub now saves the GOOD SR into _g_sr as its
; FIRST action (move.w sr,(g_sr), which does not touch CCR) -- see
; emit_save_sr in tools/shinobi_xlate.c.  block_enter restores _g_sr as before.
_exit_thunk:
                move.l  a7,(_g_sp)          ; save guest stack pointer
                move.l  (_g_host_sp),a7     ; back to host stack
                movem.l d0-d7/a0-a6,(_g_dregs)
                movem.l (a7)+,d2-d7/a2-a6   ; restore C callee-saved registers
                rts                         ; return to the C dispatcher

; shinobi_clear_icache() now lives in C (shinobi_dyntrans_amiga.c): it calls Exec
; CacheClearU(), which flushes the cache in a USER-mode-legal way (movec CACR is
; privileged and would fault now that the translator runs in user mode).

; void shinobi_super_reset(void)
;   USER-MODE: nothing to do. There is no real supervisor SR to restore (the guest
;   runs in user mode with a virtual system byte); the old `move #$2700,sr` was a
;   privileged instruction that would now fault. Kept as a no-op for the C callers.
_shinobi_super_reset:
                rts
