/*
 * shinobi_xlate.c — shared 68000->68EC020 translation core.  See shinobi_xlate.h.
 *
 * The decoder is a compact 68000 instruction LENGTH + EA classifier.  It is not a
 * full disassembler; it computes exactly what the translator needs:
 *   (1) the instruction byte length (so we can copy it verbatim),
 *   (2) the location + kind of every ABSOLUTE EA that must be +base rebased
 *       (abs.w widened to abs.l, abs.l add-in-place, and `move(a).l #imm,An`
 *        immediates that load an ADDRESS into an address register), and
 *   (3) the block terminator class + static target.
 *
 * It is validated on host (tools/shinobi_xlate_test.c) against Musashi's
 * m68kdasm over the actual runtime-reachable instruction set, so any coverage
 * gap in the Shinobi subset is caught mechanically.
 */
#include "shinobi_xlate.h"

const char *const xt_name[XT_N] = {
    "none","bra","bcc","bsr","dbcc","jmp.abs","jsr.abs","jmp(PC)","jsr(PC)",
    "jmp(An)","jsr(An)","rts","rtr","rte","stop/trap","fallthru","UNCLASSIFIED"
};
int xt_is_dynamic(int t){
    return t==XT_JMP_IND||t==XT_JSR_IND||t==XT_RTS||t==XT_RTR||t==XT_RTE||
           t==XT_JMP_PCREL||t==XT_JSR_PCREL||t==XT_STOP;
}
int xt_is_vtable(int t){ return t==XT_JMP_IND||t==XT_JSR_IND; }

/* big-endian word fetch from the flat guest image */
static uint32_t rd16(const uint8_t *g, uint32_t gsize, uint32_t a){
    if (a+1 >= gsize) return 0xFFFF;
    return ((uint32_t)g[a]<<8) | g[a+1];
}

/* extension WORDS for an EA, given operation size in BYTES (1/2/4). */
static int ea_ext_words(int mode, int reg, int sz){
    switch (mode){
        case 0: case 1: case 2: case 3: case 4: return 0; /* Dn An (An) (An)+ -(An) */
        case 5: return 1;                                  /* (d16,An)              */
        case 6: return 1;                                  /* (d8,An,Xn) brief      */
        case 7:
            switch (reg){
                case 0: return 1;                          /* abs.w                 */
                case 1: return 2;                          /* abs.l                 */
                case 2: return 1;                          /* (d16,PC)              */
                case 3: return 1;                          /* (d8,PC,Xn) brief      */
                case 4: return (sz==4)?2:1;                /* #imm                  */
            }
    }
    return 0;
}

/* record an absolute EA needing rebase, if it is one. woff = byte offset of its
 * first extension word within the instruction; field = SRC/DST (for widening). */
static void note_abs(xdec *d, int mode, int reg, uint32_t woff, int field){
    if (mode==7 && (reg==0 || reg==1) && d->nabs < XL_MAXABS){
        d->abs[d->nabs].woff  = (uint8_t)woff;
        d->abs[d->nabs].kind  = (reg==0)?XF_ABSW:XF_ABSL;
        d->abs[d->nabs].field = (uint8_t)field;
        d->nabs++;
    } else if (mode==7 && reg==2 && d->nabs < XL_MAXABS){
        /* (d16,PC) DATA EA: statically resolvable -> rebase as abs.l so the
         * relocated (different host PC) copy still reads the right guest data.
         * PC-relative is read-only, so this is always a SOURCE field.        */
        d->abs[d->nabs].woff  = (uint8_t)woff;
        d->abs[d->nabs].kind  = XF_PCREL16;
        d->abs[d->nabs].field = XFLD_SRC;
        d->nabs++;
    }
}

/* sign-extend helpers */
static int32_t s8 (uint32_t v){ return (int32_t)(int8_t)v; }
static int32_t s16(uint32_t v){ return (int32_t)(int16_t)v; }

uint32_t xl_decode(const uint8_t *g, uint32_t gsize, uint32_t pc, xdec *d){
    d->len=2; d->term=XT_NONE; d->stgt=0xFFFFFFFF; d->nabs=0; d->tmode=0; d->treg=0;
    uint32_t w0 = rd16(g,gsize,pc);
    d->w0 = (uint16_t)w0; d->w1 = (uint16_t)rd16(g,gsize,pc+2);
    int op = (w0>>12)&0xF;
    int ext = 0;                         /* extension words after opcode word */

    switch (op){
    /* ---------------- 0x0: immediates / bit ops / movep -------------------- */
    case 0x0: {
        int mode=(w0>>3)&7, reg=w0&7;
        if (w0 & 0x0100){                /* bit 8 set: dynamic bit op OR movep */
            if (mode==1){                /* MOVEP Dn,(d16,An) / (d16,An),Dn    */
                ext = 1;                 /* one 16-bit displacement word        */
            } else {                     /* BTST/BCHG/BCLR/BSET Dn,<ea> (byte)  */
                int e = ea_ext_words(mode,reg,1);
                note_abs(d,mode,reg,2,XFLD_SRC);
                ext = e;
            }
        } else {
            int ttt=(w0>>9)&7;
            int sz=(w0>>6)&3;            /* 0 byte,1 word,2 long               */
            int szb=(sz==2)?4:(sz==1?2:1);
            if (ttt==4){                 /* static bit op #imm,<ea> (byte)     */
                if (mode==7 && reg==4){ ext=1; }       /* shouldn't happen     */
                else { note_abs(d,mode,reg,4,XFLD_SRC); ext = 1 + ea_ext_words(mode,reg,1); }
            } else {                     /* ORI/ANDI/SUBI/ADDI/EORI/CMPI       */
                int immw=(szb==4)?2:1;
                if (mode==7 && reg==4){  /* #imm,CCR / #imm,SR                 */
                    ext = immw;          /* no further EA                       */
                } else {
                    note_abs(d,mode,reg,(uint32_t)(2+immw*2),XFLD_SRC);
                    ext = immw + ea_ext_words(mode,reg,szb);
                }
            }
        }
        break; }
    /* ---------------- 0x1/0x2/0x3: MOVE / MOVEA ---------------------------- */
    case 0x1: case 0x2: case 0x3: {
        int szb = (op==0x1)?1:(op==0x2?4:2);
        int smode=(w0>>3)&7, sreg=w0&7;
        int dmode=(w0>>6)&7, dreg=(w0>>9)&7;
        int se = ea_ext_words(smode,sreg,szb);
        /* source ext words start at offset 2 */
        note_abs(d,smode,sreg,2,XFLD_SRC);
        /* detect move(a).l #imm,An  -> the immediate is an ADDRESS to rebase  */
        if (op==0x2 && dmode==1 && smode==7 && sreg==4){
            /* override: source slot is an IMM_AN long immediate at offset 2   */
            d->nabs=0;
            d->abs[0].woff=2; d->abs[0].kind=XF_IMM_AN; d->abs[0].field=XFLD_SRC;
            d->nabs=1;
        }
        /* detect movea.w #imm,An -> word immediate is a sign-extended guest
         * ADDRESS; rebase it and widen the instruction to movea.l #imm32,An.  */
        else if (op==0x3 && dmode==1 && smode==7 && sreg==4){
            d->nabs=0;
            d->abs[0].woff=2; d->abs[0].kind=XF_IMM_AN_W; d->abs[0].field=XFLD_SRC;
            d->nabs=1;
        }
        /* dest ext words start at offset 2 + 2*se */
        note_abs(d,dmode,dreg,(uint32_t)(2+2*se),XFLD_DST);
        ext = se + ea_ext_words(dmode,dreg,szb);
        break; }
    /* ---------------- 0x4: misc ------------------------------------------- */
    case 0x4: {
        if ((w0 & 0xFFC0)==0x4E80 || (w0 & 0xFFC0)==0x4EC0){   /* JSR / JMP    */
            int mode=(w0>>3)&7, reg=w0&7;
            int isjmp = (w0 & 0x0040)!=0;
            d->tmode=(uint8_t)mode; d->treg=(uint8_t)reg;
            ext = ea_ext_words(mode,reg,4);
            if (mode==7 && (reg==0||reg==1)){
                /* absolute target */
                uint32_t t = (reg==0)? (uint32_t)s16(rd16(g,gsize,pc+2))&0xFFFFFF
                                     : ((rd16(g,gsize,pc+2)<<16)|rd16(g,gsize,pc+4));
                d->stgt = t & 0xFFFFFF;
                d->term = isjmp? XT_JMP_ABS : XT_JSR_ABS;
            } else if (mode==7 && (reg==2||reg==3)){
                d->term = isjmp? XT_JMP_PCREL : XT_JSR_PCREL;
            } else {
                d->term = isjmp? XT_JMP_IND : XT_JSR_IND;
            }
            break;
        }
        switch (w0){
            case 0x4E75: d->term=XT_RTS; break;
            case 0x4E77: d->term=XT_RTR; break;
            case 0x4E73: d->term=XT_RTE; break;
            case 0x4E70: case 0x4E76: case 0x4AFC: d->term=XT_STOP; break; /* reset/trapv/illegal */
            case 0x4E72: ext=1; d->term=XT_STOP; break;   /* STOP #imm        */
            case 0x4E74: ext=1; d->term=XT_STOP; break;   /* RTD #imm (020)   */
            case 0x4E71: break;                            /* NOP              */
            default: {
                int hi = w0 & 0xFFC0;
                int mode=(w0>>3)&7, reg=w0&7;
                if ((w0 & 0xFFF8)==0x4840 || (w0 & 0xFFF8)==0x4880 ||
                    (w0 & 0xFFF8)==0x48C0 || (w0 & 0xFFF8)==0x49C0){
                    ext=0;                                 /* SWAP/EXT.W/EXT.L/EXTB.L (mode 0) */
                } else if ((w0 & 0xF1C0)==0x41C0){         /* LEA <ea>,An      */
                    note_abs(d,mode,reg,2,XFLD_SRC);
                    ext = ea_ext_words(mode,reg,4);
                } else if (hi==0x4840){                    /* PEA <ea>         */
                    note_abs(d,mode,reg,2,XFLD_SRC); ext=ea_ext_words(mode,reg,4);
                } else if ((w0 & 0xFB80)==0x4880){         /* MOVEM (mode>=2)  */
                    int szb=(w0&0x40)?4:2;
                    note_abs(d,mode,reg,4,XFLD_SRC);       /* after mask word  */
                    ext = 1 + ea_ext_words(mode,reg,szb);
                } else if ((w0 & 0xFF00)==0x4000 || (w0 & 0xFF00)==0x4200 ||
                           (w0 & 0xFF00)==0x4400 || (w0 & 0xFF00)==0x4600 ||
                           (w0 & 0xFF00)==0x4A00){          /* NEGX/CLR/NEG/NOT/TST/TAS */
                    int sz=(w0>>6)&3; int szb=(sz==2)?4:(sz==1?2:1);
                    if (sz==3) szb=2;                       /* move-from-SR etc */
                    note_abs(d,mode,reg,2,XFLD_SRC);
                    ext = ea_ext_words(mode,reg,szb);
                } else if (hi==0x4800){                     /* NBCD <ea>        */
                    note_abs(d,mode,reg,2,XFLD_SRC);
                    ext = ea_ext_words(mode,reg,1);
                } else if ((w0 & 0xF140)==0x4100){          /* CHK <ea>,Dn      */
                    note_abs(d,mode,reg,2,XFLD_SRC);
                    ext = ea_ext_words(mode,reg,2);
                } else if ((w0 & 0xFFF8)==0x4E50){          /* LINK An,#disp16  */
                    ext = 1;
                } else if ((w0 & 0xFFF8)==0x4E58){          /* UNLK An          */
                    ext = 0;
                } else if ((w0 & 0xFFF0)==0x4E40){          /* TRAP #vec        */
                    d->term=XT_STOP; ext=0;
                } else if ((w0 & 0xFFF8)==0x4E60){          /* MOVE USP         */
                    ext = 0;
                } else {                                    /* fallback: EA word*/
                    note_abs(d,mode,reg,2,XFLD_SRC);
                    ext = ea_ext_words(mode,reg,2);
                }
            }
        }
        break; }
    /* ---------------- 0x5: ADDQ/SUBQ/Scc/DBcc ----------------------------- */
    case 0x5: {
        int mode=(w0>>3)&7, reg=w0&7;
        if ((w0 & 0xF0F8)==0x50C8){               /* DBcc Dn,disp16           */
            d->term=XT_DBCC; ext=1;
            d->stgt = (pc+2 + (uint32_t)s16(rd16(g,gsize,pc+2))) & 0xFFFFFF;
        } else if ((w0 & 0xF0C0)==0x50C0){        /* Scc <ea> (byte)          */
            note_abs(d,mode,reg,2,XFLD_SRC);
            ext = ea_ext_words(mode,reg,1);
        } else {                                  /* ADDQ/SUBQ <ea>           */
            int sz=(w0>>6)&3; int szb=(sz==2)?4:(sz==1?2:1);
            note_abs(d,mode,reg,2,XFLD_SRC);
            ext = ea_ext_words(mode,reg,szb);
        }
        break; }
    /* ---------------- 0x6: Bcc/BRA/BSR ------------------------------------ */
    case 0x6: {
        int cc=(w0>>8)&0xF;
        int disp8 = w0 & 0xFF;
        uint32_t tgt;
        if (disp8==0x00){ ext=1; tgt = pc+2 + (uint32_t)s16(rd16(g,gsize,pc+2)); }
        else if (disp8==0xFF){ ext=2; tgt = pc+2 + ((rd16(g,gsize,pc+2)<<16)|rd16(g,gsize,pc+4)); }
        else { ext=0; tgt = pc+2 + (uint32_t)s8(disp8); }
        d->stgt = tgt & 0xFFFFFF;
        if (cc==0) d->term=XT_BRA;
        else if (cc==1) d->term=XT_BSR;
        else d->term=XT_BCC;
        break; }
    /* ---------------- 0x7: MOVEQ ------------------------------------------ */
    case 0x7: ext=0; break;
    /* ---------------- 0x8..0xD,0xE: ALU / shift -------------------------- */
    case 0x8: case 0x9: case 0xB: case 0xC: case 0xD: {
        int mode=(w0>>3)&7, reg=w0&7;
        int sz=(w0>>6)&3;
        int opmode=(w0>>6)&7;
        /* sz==3 (bits7-6=11) is the address-register-dest / mul-div form:
         *   0x8 DIVU/DIVS, 0xC MULU/MULS  -> WORD data EA;
         *   0x9 SUBA, 0xB CMPA, 0xD ADDA   -> opmode 011=.W (word), 111=.L (long).
         * ABCD/SBCD/ADDX/SUBX/CMPM/EXG use reg-direct or (An)+ => 0 ext anyway. */
        int szb;
        if (sz==3){
            if (op==0x8 || op==0xC) szb=2;          /* MUL/DIV word EA          */
            else szb=(opmode==7)?4:2;               /* xxxA.L : xxxA.W          */
        } else szb=(sz==2)?4:(sz==1?2:1);
        note_abs(d,mode,reg,2,XFLD_SRC);
        ext = ea_ext_words(mode,reg,szb);
        break; }
    case 0xE: {                          /* shift/rotate                       */
        int mode=(w0>>3)&7, reg=w0&7;
        if (((w0>>6)&3)==3){             /* memory shift <ea> (word)           */
            note_abs(d,mode,reg,2,XFLD_SRC);
            ext = ea_ext_words(mode,reg,2);
        } else ext=0;                    /* register shift                     */
        break; }
    /* ---------------- 0xA / 0xF: line-A / line-F (unused by Shinobi) ------ */
    case 0xA: case 0xF: default:
        d->term=XT_UNCL; d->len=2; return 2;
    }

    d->len = (uint32_t)(2 + ext*2);
    if (d->len==0 || d->len>16){ d->term=XT_UNCL; d->len=2; }
    return d->len;
}

/* ====================================================================== */
/* EMITTER                                                                  */
/* ====================================================================== */
static int put16(uint8_t *o,int *n,int cap,uint32_t v){
    if (*n+2>cap) return 0; o[(*n)++]=(uint8_t)(v>>8); o[(*n)++]=(uint8_t)v; return 1;
}
static int put32(uint8_t *o,int *n,int cap,uint32_t v){
    if (*n+4>cap) return 0;
    o[(*n)++]=(uint8_t)(v>>24); o[(*n)++]=(uint8_t)(v>>16);
    o[(*n)++]=(uint8_t)(v>>8);  o[(*n)++]=(uint8_t)v; return 1;
}

/* emit a non-terminator instruction with absolute EAs rebased to +base */
int xl_emit_instr(const uint8_t *g, uint32_t gsize, uint32_t pc,
                  const xdec *d, const xl_emit_env *env,
                  uint8_t *out, int cap){
    int n=0;
    uint32_t w0 = rd16(g,gsize,pc);

    /* ---- USER-MODE-SAFE virtualisation of the guest's PRIVILEGED SR instructions ----
     * The translated blocks run in USER mode (so the OS/RTG graphics path works). The
     * real 020 SR is therefore user-mode (CCR only); the guest's system byte (S/IPL)
     * lives VIRTUALLY in the g_sr cell. move/andi/ori/eori-to-SR -> ops on (g_sr);
     * move-from-SR -> read (g_sr). CCR is the real CCR (carried as before). g_sr (env
     * field gregs_sr) is the same cell block_enter restores into CCR. */
    if (w0 == 0x46FC) {                                  /* move #imm,SR */
        uint32_t imm = rd16(g,gsize,pc+2);
        if(!put16(out,&n,cap,0x33FC))return 0; if(!put16(out,&n,cap,imm))return 0; if(!put32(out,&n,cap,env->gregs_sr))return 0; /* move.w #imm,(g_sr).l */
        if(!put16(out,&n,cap,0x44FC))return 0; if(!put16(out,&n,cap,imm))return 0;  /* move #imm,CCR (low byte) */
        return n;
    }
    if (w0 == 0x027C || w0 == 0x007C || w0 == 0x0A7C) { /* andi/ori/eori #imm,SR */
        uint32_t imm = rd16(g,gsize,pc+2);
        uint16_t op = (w0==0x027C)?0x0279 : (w0==0x007C)?0x0079 : 0x0A79; /* ...i.w #imm,(g_sr).l */
        if(!put16(out,&n,cap,op))return 0; if(!put16(out,&n,cap,imm))return 0; if(!put32(out,&n,cap,env->gregs_sr))return 0;
        return n;
    }
    if ((w0 & 0xFFF8) == 0x40C0) {                       /* move SR,Dn  ->  move.w (g_sr),Dn */
        int dreg = w0 & 7;
        if(!put16(out,&n,cap,(uint32_t)(0x3000 | (dreg<<9) | 0x39)))return 0;
        if(!put32(out,&n,cap,env->gregs_sr))return 0;
        return n;
    }
    if ((w0 & 0xFFF8) == 0x46C0) {                       /* move Dn,SR  ->  (g_sr)=Dn ; CCR=(g_sr) */
        int sreg = w0 & 7;
        if(!put16(out,&n,cap,(uint32_t)(0x33C0 | sreg)))return 0; if(!put32(out,&n,cap,env->gregs_sr))return 0;
        if(!put16(out,&n,cap,0x44F9))return 0; if(!put32(out,&n,cap,env->gregs_sr))return 0;
        return n;
    }

    /* patch opcode word EA fields for any abs.w / (d16,PC) we widen to abs.l */
    for (int i=0;i<d->nabs;i++){
        if (d->abs[i].kind==XF_ABSW){
            if (d->abs[i].field==XFLD_SRC){ w0 = (w0 & ~0x3F) | 0x39; }  /* 7/0 -> 7/1 src */
            else                          { w0 |= (1u<<9); }            /* dst reg 0 -> 1 */
        } else if (d->abs[i].kind==XF_PCREL16){
            /* (d16,PC) is mode 7 reg 2 in the SOURCE EA field -> abs.l 7/1 */
            w0 = (w0 & ~0x3F) | 0x39;
        } else if (d->abs[i].kind==XF_IMM_AN_W){
            /* movea.w (size bits13-12 = 11) -> movea.l (10): widen + rebase   */
            w0 = (w0 & ~0x3000) | 0x2000;
        }
    }
    if (!put16(out,&n,cap,w0)) return 0;

    /* walk extension words, rewriting the abs slots */
    uint32_t boff = 2;                              /* byte offset into instr  */
    while (boff < d->len){
        int handled=0;
        for (int i=0;i<d->nabs;i++){
            if (d->abs[i].woff != boff) continue;
            if (d->abs[i].kind==XF_ABSW){
                uint32_t w = rd16(g,gsize,pc+boff);
                if (!put32(out,&n,cap, ((uint32_t)s16(w) & 0xFFFFFF) + env->base)) return 0;
                boff += 2;                          /* consumed 1 input word   */
            } else if (d->abs[i].kind==XF_ABSL){
                uint32_t a = (rd16(g,gsize,pc+boff)<<16)|rd16(g,gsize,pc+boff+2);
                if (!put32(out,&n,cap,(a & 0xFFFFFF) + env->base)) return 0;
                boff += 4;
            } else if (d->abs[i].kind==XF_PCREL16){
                /* resolve (d16,PC): EA = address-of-disp-word + sign16(disp).
                 * The disp word lives at guest pc+boff, so resolve to an
                 * absolute guest address and rebase (widens 1 word -> 2). */
                uint32_t disp = rd16(g,gsize,pc+boff);
                uint32_t resolved = (pc + boff + (uint32_t)s16(disp)) & 0xFFFFFF;
                if (!put32(out,&n,cap, resolved + env->base)) return 0;
                boff += 2;                          /* consumed 1 input word   */
            } else if (d->abs[i].kind==XF_IMM_AN_W){
                /* movea.w #imm16,An : sign-extend the word to a 24-bit guest
                 * address, rebase, emit as the 32-bit movea.l immediate. */
                uint32_t a = (uint32_t)s16(rd16(g,gsize,pc+boff)) & 0xFFFFFF;
                if (!put32(out,&n,cap, a + env->base)) return 0;
                boff += 2;                          /* consumed 1 input word   */
            } else { /* XF_IMM_AN: long immediate that is an address */
                uint32_t a = (rd16(g,gsize,pc+boff)<<16)|rd16(g,gsize,pc+boff+2);
                if (!put32(out,&n,cap,(a & 0xFFFFFF) + env->base)) return 0;
                boff += 4;
            }
            handled=1; break;
        }
        if (handled) continue;
        if (!put16(out,&n,cap,rd16(g,gsize,pc+boff))) return 0;  /* copy verbatim */
        boff += 2;
    }
    return n;
}

/* Bcc condition code -> the same cc in a fresh Bcc.W opcode (0x6000 | cc<<8). */
/* helper stubs used by the terminator -------------------------------------- */
static int emit_set_pc_imm(uint8_t *o,int *n,int cap,const xl_emit_env*e,uint32_t v){
    /* move.l #v,(gregs_pc).L */
    if(!put16(o,n,cap,0x23FC))return 0; if(!put32(o,n,cap,v))return 0;
    return put32(o,n,cap,e->gregs_pc);
}
static int emit_jmp_exit(uint8_t *o,int *n,int cap,const xl_emit_env*e){
    if(!put16(o,n,cap,0x4EF9))return 0; return put32(o,n,cap,e->exit_thunk); /* jmp (xxx).L */
}
/* Save the GOOD guest SR (incl. CCR set by the block body) into the g_sr cell.
 * MUST be the FIRST action of every terminator stub, BEFORE any flag-clobbering
 * instruction (e.g. the `move.l #pc,(g_pc)` pc-set) — otherwise the next block,
 * which restores g_sr into its CCR via block_enter, reads garbage flags and a
 * leading conditional branch takes the wrong path.  `move.w sr,<ea>` does NOT
 * touch CCR, so the subsequent Bcc still sees the correct flags.  (g_sr is
 * re-saved here, NOT in exit_thunk, which used to capture the post-stub CCR.) */
static int emit_save_sr(uint8_t *o,int *n,int cap,const xl_emit_env*e){
    /* USER-MODE: carry only the CCR forward via move-FROM-CCR (0x42F9, user-legal on
     * 020), which reads CCR WITHOUT touching it -- a trailing Bcc still sees the right
     * flags, like the old privileged `move sr,ea`.  move.w ccr,(gregs_sr).L.          */
    if(!put16(o,n,cap,0x42F9))return 0; return put32(o,n,cap,e->gregs_ccr);
}

/* Emit a conditional terminator (Bcc/DBcc). Layout (all offsets relative to the
 * branch, which follows the leading 6-byte save-SR):
 *     +0   <cond-branch>  Dn?,disp16=+18  -> .taken
 *     +4   set_pc(fall) ; jmp exit          (fall arm = 16 bytes)
 *     +20 .taken: set_pc(target) ; jmp exit
 * disp16 is PC-relative to the displacement word (branch+2): 20-2 = 18.  The
 * save-SR prefix shifts branch and arms together, so disp16 is unaffected. */
static int emit_cond(uint8_t *o,int *n,int cap,const xdec*d,uint32_t fall,
                     const xl_emit_env*e){
    int cc = (d->w0>>8)&0xF;
    /* Save SR FIRST (does not touch CCR) so this block's flags reach the next
     * block; the Bcc below still reads the correct CCR from the block body. */
    if(!emit_save_sr(o,n,cap,e))return 0;
    if (d->term==XT_DBCC){
        if(!put16(o,n,cap,0x50C8 | (cc<<8) | (d->w0&7)))return 0;   /* DBcc Dn  */
    } else {
        if(!put16(o,n,cap,0x6000 | (cc<<8)))return 0;              /* Bcc.W    */
    }
    if(!put16(o,n,cap,18))return 0;                                 /* disp16   */
    /* fall arm */
    if(!emit_set_pc_imm(o,n,cap,e,fall))return 0;                   /* 10 bytes */
    if(!emit_jmp_exit(o,n,cap,e))return 0;                          /* 6 bytes  */
    /* taken arm */
    if(!emit_set_pc_imm(o,n,cap,e,d->stgt))return 0;
    if(!emit_jmp_exit(o,n,cap,e))return 0;
    return *n;
}

int xl_emit_term(uint32_t pc, const xdec *d, uint32_t fall,
                 const xl_emit_env *env, uint8_t *out, int cap){
    int n=0;
    (void)pc;
    switch (d->term){
    case XT_BRA: case XT_JMP_ABS:
        if(!emit_save_sr(out,&n,cap,env))return 0;      /* carry CCR forward    */
        if(!emit_set_pc_imm(out,&n,cap,env,d->stgt))return 0;
        if(!emit_jmp_exit(out,&n,cap,env))return 0;
        break;
    case XT_BCC: case XT_DBCC:
        return emit_cond(out,&n,cap,d,fall,env);
    case XT_BSR: case XT_JSR_ABS:
        /* save SR ; pea (fall).L  (push GUEST return pc) ; set_pc(target) ; jmp */
        if(!emit_save_sr(out,&n,cap,env))return 0;
        if(!put16(out,&n,cap,0x4879))return 0; if(!put32(out,&n,cap,fall))return 0;
        if(!emit_set_pc_imm(out,&n,cap,env,d->stgt))return 0;
        if(!emit_jmp_exit(out,&n,cap,env))return 0;
        break;
    case XT_RTS:
        /* save SR ; move.l (a7)+,(gregs_pc).L ; jmp exit */
        if(!emit_save_sr(out,&n,cap,env))return 0;
        if(!put16(out,&n,cap,0x23DF))return 0; if(!put32(out,&n,cap,env->gregs_pc))return 0;
        if(!emit_jmp_exit(out,&n,cap,env))return 0;
        break;
    case XT_RTR:
        if(!emit_save_sr(out,&n,cap,env))return 0;       /* (addq below clobbers CCR) */
        if(!put16(out,&n,cap,0x548F))return 0;          /* addq.l #2,a7 (CCR)  */
        if(!put16(out,&n,cap,0x23DF))return 0; if(!put32(out,&n,cap,env->gregs_pc))return 0;
        if(!emit_jmp_exit(out,&n,cap,env))return 0;
        break;
    case XT_RTE:
        /* The guest is 68000 code: its exception frames are the 68000 6-byte
         * form [SR.w][PC.l] (it even builds them itself, e.g. pea/move.w/rte).
         * Pop SR into the REAL sr (privileged, supervisor), THEN save THAT
         * restored guest SR into the g_sr cell so the interrupted block's flags
         * (and IPL) carry forward — exit_thunk no longer saves SR.  Saving AFTER
         * the pop is what makes RTE differ from the other stubs: here the value
         * that must propagate is the popped guest SR, not the pre-stub SR.  Then
         * pop PC into g_pc (move.l clobbers CCR, but g_sr is already saved).  NO
         * 020 format word; inject_irq4 pushes the matching 6-byte frame. */
        /* USER-MODE: pop the 6-byte 68000 frame [SR.w][PC.l] WITHOUT a privileged
         * move-to-SR. Pop SR straight into the virtual g_sr cell (full word: system
         * byte + CCR); block_enter restores CCR from g_sr's low byte when the next
         * block runs. Then pop PC into g_pc. */
        if(!put16(out,&n,cap,0x33DF))return 0; if(!put32(out,&n,cap,env->gregs_sr))return 0; /* move.w (a7)+,(g_sr).l */
        if(!put16(out,&n,cap,0x33F9))return 0; if(!put32(out,&n,cap,env->gregs_sr))return 0; if(!put32(out,&n,cap,env->gregs_ccr))return 0; /* move.w (g_sr),(g_ccr): carry restored CCR */
        if(!put16(out,&n,cap,0x23DF))return 0; if(!put32(out,&n,cap,env->gregs_pc))return 0; /* move.l (a7)+,(g_pc).l */
        if(!emit_jmp_exit(out,&n,cap,env))return 0;
        break;
    case XT_JMP_IND: case XT_JSR_IND:
        if(!emit_save_sr(out,&n,cap,env))return 0;       /* carry CCR forward    */
        if (d->term==XT_JSR_IND){       /* push GUEST return pc                */
            if(!put16(out,&n,cap,0x4879))return 0; if(!put32(out,&n,cap,fall))return 0;
        }
        if (d->tmode==2){               /* (An): target = aN                   */
            if(!put16(out,&n,cap,0x23C8 | d->treg))return 0;       /* move.l aN */
            if(!put32(out,&n,cap,env->gregs_pc))return 0;
        } else if (d->tmode==5){        /* (d16,An): target = aN + d16         */
            if(!put16(out,&n,cap,0x23C8 | d->treg))return 0;       /* move.l aN */
            if(!put32(out,&n,cap,env->gregs_pc))return 0;
            if(!put16(out,&n,cap,0x06B9))return 0;                 /* addi.l #  */
            if(!put32(out,&n,cap,(uint32_t)(int32_t)(int16_t)d->w1))return 0;
            if(!put32(out,&n,cap,env->gregs_pc))return 0;
        } else {
            return 0;                   /* (d8,An,Xn) etc -> runtime fault stub */
        }
        if(!emit_jmp_exit(out,&n,cap,env))return 0;
        break;
    case XT_JMP_PCREL: case XT_JSR_PCREL: {
        /* Computed jump through a (d8,PC,Xn) brief-format EA — the attract /
         * object state-machine dispatcher (e.g. `jmp ($2,PC,D2.l)` @0x29da, a
         * table of bra entries indexed by an object's state byte).  After the
         * code is relocated the host PC differs from the guest PC, so a verbatim
         * PC-relative jump derails; resolve it to a GUEST target arithmetically:
         *     target = (pc+2) + sign8(d8) + Xn(scaled)
         * The 68000 PC value for a brief-format EA is the address of the
         * extension word = pc+2.  We emit code that recomputes the guest target
         * into g_pc from the LIVE index register and dispatches it.
         *
         * Only the common jump-table form is emitted: a DATA-register index,
         * LONG size, scale 1 (what every Shinobi computed jump uses).  Any other
         * form (An index / word index / scaled / full-format) returns 0 so the
         * runtime falls back to a fault stub, exactly as before. */
        uint16_t e = d->w1;
        if (e & 0x0100) return 0;                 /* 020 full ext format: skip   */
        int idx_a   = (e >> 15) & 1;              /* 0=Dn  1=An                  */
        int idx_reg = (e >> 12) & 7;
        int idx_long= (e >> 11) & 1;              /* 0=word 1=long               */
        int scale   = (e >> 9)  & 3;              /* 0=x1                        */
        int disp8   = (int8_t)(e & 0xFF);
        if (idx_a || !idx_long || scale != 0) return 0;
        uint32_t basev = (pc + 2 + (uint32_t)(int32_t)disp8) & 0xFFFFFF;
        if(!emit_save_sr(out,&n,cap,env))return 0;       /* carry CCR forward    */
        if (d->term == XT_JSR_PCREL){             /* push GUEST return pc         */
            if(!put16(out,&n,cap,0x4879))return 0; if(!put32(out,&n,cap,fall))return 0;
        }
        /* move.l #basev,(g_pc).L */
        if(!put16(out,&n,cap,0x23FC))return 0; if(!put32(out,&n,cap,basev))return 0;
        if(!put32(out,&n,cap,env->gregs_pc))return 0;
        /* add.l Dn,(g_pc).L   (0xD1B9 | reg<<9 = ADD.L Dn,(abs).L) */
        if(!put16(out,&n,cap,(uint16_t)(0xD1B9 | (idx_reg<<9))))return 0;
        if(!put32(out,&n,cap,env->gregs_pc))return 0;
        if(!emit_jmp_exit(out,&n,cap,env))return 0;
        break; }
    case XT_STOP:
    default:
        return 0;                        /* runtime emits a fault stub          */
    }
    return n;
}

int xl_emit_fault(uint32_t pc, const xl_emit_env *env, uint8_t *out, int cap){
    int n=0;
    if(!emit_save_sr(out,&n,cap,env))return 0;           /* carry CCR forward    */
    /* move.l #pc,(g_fault_pc).L */
    if(!put16(out,&n,cap,0x23FC))return 0; if(!put32(out,&n,cap,pc))return 0;
    if(!put32(out,&n,cap,env->fault_pc))return 0;
    /* move.l #sentinel,(g_pc).L ; jmp exit */
    if(!emit_set_pc_imm(out,&n,cap,env,env->fault_sentinel))return 0;
    if(!emit_jmp_exit(out,&n,cap,env))return 0;
    return n;
}
