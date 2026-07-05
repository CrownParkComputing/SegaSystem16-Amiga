; src/amiga/amiga.s
; ============================================================
;  amiga_main -- the HAL entry point invoked by the WHDLoad slave
;  OR called directly as a CLI command.
; ============================================================
;
; On entry (WHDLoad slave case):
;   sp+4 = ExecBase (a6 was pushed by slave)
; On entry (CLI case):
;   a6 = ExecBase (passed by AmigaOS startup)
;   a4 may or may not be set (small-data base)
;
; We use the 68k asm to:
;   1. Set up SysBase (libnix-style: store a6 at _SysBase)
;   2. Allocate a small stack frame
;   3. Call the C-side hal_video_open (does AllocMem + chipset)
;   4. Enter a "wait for left mouse" loop so the screen is
;      visible until the user dismisses it
;   5. Return cleanly
;
; The interpreter cores (m6809, z80) and dispatch table are
; already linked in. Once the 6809 ROMs are loaded and
; dispatched, the per-frame orchestrator goes in here.
; ============================================================

        XDEF    amiga_main
        XREF    _SysBase
        XREF    _hal_game_init
        XREF    _hal_game_frame
        XREF    _hal_game_should_exit
        XREF    _hal_cleanup

        SECTION code,CODE

        SECTION bss,BSS

cpubase_save:  ds.l   1               ; execbase slot
wbmsg:         ds.l   1               ; WBStartupMessage (0 if CLI-launched)
oldsp:         ds.l   1               ; SP before the stack swap
newstack:      ds.l   1               ; AllocMem'd stack base (0 = inherited stack)

        SECTION code,CODE

amiga_main:
        ; Load ExecBase the canonical way -- from absolute address 4.
        ; a6 is ExecBase under the WHDLoad slave contract, but NOT when
        ; we're launched as a normal CLI program from Startup-Sequence,
        ; so we must not trust it. Reading 4.w works in both cases.
        ; (Trusting a6 here caused error #80000004 -- illegal instruction
        ;  -- when AllocMem jumped through a garbage _SysBase.)
        move.l  4.w,a6
        ; Set _SysBase so the libnix-style C code can use AllocMem
        ; and other Exec functions. The symbol is declared in
        ; proto/exec.h.
        move.l  a6,_SysBase

        ; Stash registers, set up a small frame.
        link    a5,#-8
        movem.l d0-d7/a0-a5,-(sp)
        move.l  a6,cpubase_save

        ; Workbench launch (AGS icon double-click): capture the startup
        ; message now and reply it on exit. Returning without replying this
        ; message is a common cause of Workbench software failure #87000004.
        moveq   #0,d0
        move.l  d0,wbmsg
        sub.l   a1,a1
        jsr     -294(a6)              ; FindTask(NULL) -> d0 = our Process
        move.l  d0,a4
        tst.l   172(a4)               ; pr_CLI != 0 -> CLI launch
        bne.s   .clilaunch
        lea     92(a4),a3             ; &pr_MsgPort
        move.l  a3,a0
        jsr     -384(a6)              ; WaitPort(port)
        move.l  a3,a0
        jsr     -372(a6)              ; GetMsg(port) -> d0 = WBStartupMessage
        move.l  d0,wbmsg
.clilaunch:

        ; Run the interpreter and runtime ROM loader on a known-large stack
        ; instead of whatever stack the icon or shell happened to provide.
        move.l  #$40000,d0            ; 256 KB
        moveq   #0,d1                 ; MEMF_ANY
        jsr     -198(a6)              ; AllocMem -> d0 = base (0 = failed)
        move.l  d0,newstack
        beq.s   .nostack
        move.l  sp,oldsp
        add.l   #$3fff8,d0            ; top of 256 KB block, 8-byte aligned
        move.l  d0,sp
.nostack:

        ; One-time game init (sets up video, loads ROMs, resets CPU).
        ; Absolute call via a register (move.l #sym -> 32-bit reloc): with the
        ; native transcode merged into the entry code hunk, _hal_game_init is
        ; >32KB away -- out of bsr.w / (d16,PC) range.
        move.l  #_hal_game_init,a0
        jsr     (a0)

        moveq   #0,d7                 ; d7 = left-mouse HOLD counter (frames)
                                      ; (C funcs preserve d2-d7, so it survives the bsr)
.loop:
        ; Run + render one frame.
        move.l  #_hal_game_frame,a0
        jsr     (a0)
        move.l  #_hal_game_should_exit,a0
        jsr     (a0)
        tst.l   d0
        bne.s   .exit

        ; QUIT only on a DELIBERATE left-mouse HOLD (~3s), not a single click.
        ; A brief click in the Amiberry window used to fall straight through to
        ; Exit -- but we leave the chipset taken over (Forbid + our DMA/copper),
        ; so the display stuck = "freeze on click". Requiring a long hold means a
        ; normal click does nothing; only an intentional press-and-hold exits.
        ; CIA-A PRA $bfe001 bit6 = port-0 fire = LEFT MOUSE BUTTON (active low).
        move.b  $bfe001,d0
        andi.b  #$40,d0               ; bit 6 = 0 means pressed
        bne.s   .notheld              ; released -> reset the hold counter
        addq.l  #1,d7
        cmpi.l  #150,d7               ; ~150 frames (~3s @ 50Hz) held -> quit
        bcc.s   .exit
        bra.s   .loop
.notheld:
        moveq   #0,d7
        bra.s   .loop

.exit:
        ; Exit through the C teardown while the private stack is still active.
        move.l  #_hal_cleanup,a0
        jsr     (a0)
        move.w  #$000f,$dff096        ; DMACON: clear AUD0-3 DMA
        move.l  cpubase_save,a6       ; ExecBase for FreeMem/ReplyMsg
        move.l  newstack,d0
        beq.s   .nofree
        move.l  oldsp,sp              ; restore inherited stack first
        move.l  newstack,a1
        move.l  #$40000,d0
        jsr     -210(a6)              ; FreeMem(newstack, 256 KB)
.nofree:
        move.l  wbmsg,d0
        beq.s   .noreply
        jsr     -132(a6)              ; Forbid()
        move.l  wbmsg,a1
        jsr     -378(a6)              ; ReplyMsg(wbmsg)
.noreply:
        movem.l (sp)+,d0-d7/a0-a5
        unlk    a5
        moveq   #0,d0
        rts

        END
