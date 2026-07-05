/*
 * shinobi_rebase.c — rebase-recompiler ANALYSIS pass for Shinobi (System 16B).
 *
 * Recursive-descent disassembles the 256KB program image from the live entry
 * points (reset 0x400, IRQ4 handler 0x2684, and the live exception vectors),
 * building the set of statically-reachable instruction addresses.  For every
 * instruction it CLASSIFIES the memory operands by target region:
 *
 *   ROM        0x000000-0x03FFFF      -> rebase (+base)
 *   VRAM       tile/text/sprite/pal   -> rebase (+base), renderer scans
 *   WORKRAM    0xFF0000-0xFFFFFF      -> rebase (+base)   (incl. abs.w $xxxx.w)
 *   MMIO       0xC4xxxx/0xC60000/
 *              0x3F0000/0xFE00xx      -> REWRITE to a HAL call
 *
 * and records the ADDRESSING FORM (abs.l / abs.w / PC-relative / register-
 * indirect) so we can tell which MMIO accesses are point-patchable (absolute
 * EA) vs which need base-load trapping (register-indirect).
 *
 * Build:
 *   cc -O2 -I src/cores/m68k -o /tmp/shinobi_rebase \
 *       tools/shinobi_rebase.c src/cores/m68k/m68kdasm.c
 * Run:
 *   /tmp/shinobi_rebase games/shinobi/roms/shinobi_main.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "m68k.h"

#define ROMSZ 0x40000
static uint8_t BUF[ROMSZ];
static unsigned BUFSZ = 0;

/* visited[a] : 1 = decoded as an instruction start */
static uint8_t visited[ROMSZ];
/* covered[a] : 1 = byte is part of some decoded instruction (to find tails) */
static uint8_t covered[ROMSZ];

/* worklist */
static unsigned wl[ROMSZ];
static int wl_n = 0;
static void push(unsigned a){ if (a<BUFSZ && !visited[a]) wl[wl_n++] = a; }

/* candidate code entry points harvested from absolute immediates that land in
 * the ROM code range — these are the handler addresses the game stores into
 * RAM object records via `move.l #$romaddr,(d,An)` and pushes via pea. */
static unsigned cand[ROMSZ]; static int cand_n=0;
static uint8_t cand_seen[ROMSZ];
static void add_cand(unsigned a){
    if (a>=0x400 && a<BUFSZ && !(a&1) && !cand_seen[a]) { cand_seen[a]=1; cand[cand_n++]=a; }
}

unsigned int m68k_read_disassembler_8 (unsigned int a){ return a<BUFSZ?BUF[a]:0; }
unsigned int m68k_read_disassembler_16(unsigned int a){ return a+1<BUFSZ?(BUF[a]<<8)|BUF[a+1]:0; }
unsigned int m68k_read_disassembler_32(unsigned int a){ return a+3<BUFSZ?(BUF[a]<<24)|(BUF[a+1]<<16)|(BUF[a+2]<<8)|BUF[a+3]:0; }

/* ---- region classification ---- */
enum { R_ROM, R_TILE, R_TEXT, R_SPR, R_PAL, R_WORK, R_MMIO_IO, R_MMIO_WD,
       R_MMIO_CMD, R_MMIO_MAP, R_OTHER, R_NREG };
static const char *RNAME[R_NREG] = {
    "ROM","tileram","textram","spriteram","paletteram","workram",
    "MMIO:io","MMIO:watchdog","MMIO:cmdwin","MMIO:mapper","other" };
static int region_of(unsigned a){
    a &= 0xFFFFFF;
    if (a < 0x040000) return R_ROM;
    if (a >= 0x3F0000 && a < 0x400000) return R_MMIO_CMD;
    if (a >= 0x400000 && a < 0x410000) return R_TILE;
    if (a >= 0x410000 && a < 0x411000) return R_TEXT;
    if (a >= 0x440000 && a < 0x440800) return R_SPR;
    if (a >= 0x840000 && a < 0x841000) return R_PAL;
    if (a >= 0xC40000 && a < 0xC44000) return R_MMIO_IO;
    if (a >= 0xC60000 && a < 0xC60002) return R_MMIO_WD;
    if (a >= 0xFE0000 && a < 0xFE0040) return R_MMIO_MAP;
    if (a >= 0xFF0000)                 return R_WORK;
    return R_OTHER;
}
static int is_mmio(int r){ return r==R_MMIO_IO||r==R_MMIO_WD||r==R_MMIO_CMD||r==R_MMIO_MAP; }

/* ---- stats ---- */
static long n_instr = 0;
static long n_abs_l = 0, n_abs_w = 0, n_pcrel = 0, n_reg_ind = 0;
static long region_hits[R_NREG];        /* memory operands (abs) per region    */
static long mmio_instr = 0;             /* instructions touching MMIO          */
static long region_instr[R_NREG];       /* instructions touching each region   */

/* computed-jump / jump-table sites */
static unsigned cjmp[4096]; static int cjmp_n=0;
static char    cjmp_txt[4096][80];

/* MMIO instruction catalogue (unique forms) */
struct mform { char text[96]; unsigned addr; long count; };
static struct mform mforms[2048]; static int mforms_n=0;
static void add_mform(const char *t, unsigned a){
    for (int i=0;i<mforms_n;i++) if (!strcmp(mforms[i].text,t)) { mforms[i].count++; return; }
    if (mforms_n<2048){ strncpy(mforms[mforms_n].text,t,95); mforms[mforms_n].addr=a; mforms[mforms_n].count=1; mforms_n++; }
}

static int is_hex(char c){ return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
static unsigned hexval(const char **pp){
    unsigned v=0; int n=0; const char *p=*pp;
    while (n<8 && is_hex(*p)){ char c=*p++; v=(v<<4)|(c<='9'?c-'0':(c|0x20)-'a'+10); n++; }
    *pp=p; return n?v:0xFFFFFFFFu;
}

/* Parse a disassembly line for memory-operand addressing forms + classify.
 * Returns a bitmask of regions touched by ABSOLUTE operands; also flags
 * register-indirect / PC-relative presence. */
static void classify_line(unsigned pc, const char *dis)
{
    int touched[R_NREG]; memset(touched,0,sizeof touched);
    int saw_mmio=0;
    const char *p = dis;

    /* scan every '$' token; decide its addressing form by the suffix */
    while ((p = strchr(p,'$')) != NULL) {
        const char *q = p+1;
        unsigned a = hexval(&q);
        if (a==0xFFFFFFFFu) { p++; continue; }
        /* suffix tells the form:
         *  "$xxxx.l"            absolute long
         *  "$xxxx.w"            absolute short (sign-extend to 24-bit)
         *  "$xxxx(PC..."        -> handled below; Musashi prints "($d,PC...)"
         */
        char s0 = q[0], s1 = q[1];
        int is_l = (s0=='.' && (s1=='l'||s1=='L'));
        int is_w = (s0=='.' && (s1=='w'||s1=='W'));
        /* Is this token an index/disp inside a (...,PC...) or (...,An...) group?
         * Look backwards: an immediate is "#$..", a disp inside parens starts
         * with '(' just before '$'. We treat '(' before '$' as register/PC EA. */
        int in_paren = (p>dis && p[-1]=='(');
        int is_imm   = (p>dis && p[-1]=='#');
        if (is_l || is_w) {
            unsigned ea = a;
            if (is_w && (a & 0x8000)) ea = 0xFF0000 | (a & 0xFFFF); /* sign-extend short */
            else if (is_w)            ea = a & 0xFFFF;
            int r = region_of(ea);
            touched[r]=1;
            if (is_l) n_abs_l++; else n_abs_w++;
            region_hits[r]++;
            if (is_mmio(r)) { saw_mmio=1; add_mform(dis, pc); }
        } else if (is_imm) {
            /* immediate constant — harvest ROM-range values as candidate code
             * entry points (stored handler pointers / pea targets). Only long
             * immediates ($xxxxxx, >=5 hex digits) to avoid byte/word data. */
            if (a>=0x400 && a<BUFSZ) {
                const char *r=q; int nd=0; const char *back=p+1;
                while (is_hex(*back)){nd++;back++;}
                if (nd>=5) add_cand(a);
            }
        } else if (in_paren) {
            /* displacement inside an EA group; the group decides the form.
             * We detect PC-rel vs reg-ind by scanning the group text. */
        }
        p = q;
    }
    /* addressing-form presence (independent of classification) */
    if (strstr(dis,",PC)") || strstr(dis,",PC,") || strstr(dis,"(PC)")) n_pcrel++;
    /* register-indirect: "(A0)","(A7)+","-(A1)","($x,A0)","($x,A0,D1.w)" */
    {
        const char *r=dis;
        while ((r=strchr(r,'(')) != NULL) {
            /* find a 'A<digit>' before the matching ')' that is NOT 'PC' */
            const char *e = strchr(r,')'); if(!e) break;
            int has_an=0;
            for (const char *t=r; t<e; t++)
                if (t[0]=='A' && t[1]>='0'&&t[1]<='7') has_an=1;
            if (has_an){ n_reg_ind++; }
            r=e+1;
        }
    }
    for (int i=0;i<R_NREG;i++) if (touched[i]) region_instr[i]++;
    if (saw_mmio) mmio_instr++;
}

static void drain(void)
{
    while (wl_n>0) {
        unsigned pc = wl[--wl_n];
        while (pc<BUFSZ && !visited[pc]) {
            char dis[256]={0};
            unsigned len = m68k_disassemble_raw(dis,pc,BUF+pc,NULL,M68K_CPU_TYPE_68000);
            if (len==0||len>16) { break; }
            visited[pc]=1;
            for (unsigned i=0;i<len && pc+i<BUFSZ;i++) covered[pc+i]=1;
            n_instr++;
            classify_line(pc,dis);

            /* control flow */
            char mnem[16]={0}; sscanf(dis,"%15s",mnem);
            int stop_fallthru=0;
            /* extract a branch/jump absolute target ("$xxxx" or "$xxxx.l") that
             * is NOT inside parens/# (i.e. a direct branch displacement target) */
            unsigned tgt=0xFFFFFFFF;
            {
                const char *p=strchr(dis,'$');
                /* for control-flow ops the FIRST $ token is the target */
                if (p && (p==dis || (p[-1]!='#' && p[-1]!='('))) {
                    const char *q=p+1; tgt=hexval(&q);
                }
            }
            if (!strcmp(mnem,"rts")||!strcmp(mnem,"rte")||!strcmp(mnem,"rtr")||
                !strcmp(mnem,"reset")) { stop_fallthru=1; }
            else if (!strcmp(mnem,"bra")) {
                if (tgt!=0xFFFFFFFF) push(tgt); stop_fallthru=1;
            }
            else if (!strcmp(mnem,"jmp")) {
                /* jmp $abs.l -> follow; jmp (An)/(d,An,Xn)/(d,PC,Xn) -> computed */
                if (strchr(dis,'(')) {
                    if (cjmp_n<4096){ cjmp[cjmp_n]=pc; strncpy(cjmp_txt[cjmp_n],dis,79); cjmp_n++; }
                } else if (tgt!=0xFFFFFFFF) push(tgt);
                stop_fallthru=1;
            }
            else if (!strcmp(mnem,"jsr")) {
                if (strchr(dis,'(')) {
                    if (cjmp_n<4096){ cjmp[cjmp_n]=pc; strncpy(cjmp_txt[cjmp_n],dis,79); cjmp_n++; }
                } else if (tgt!=0xFFFFFFFF) push(tgt);
                /* subroutine returns -> fall through */
            }
            else if (mnem[0]=='b' && strlen(mnem)>=3) {
                /* Bcc / bsr : take target AND fall through */
                if (tgt!=0xFFFFFFFF) push(tgt);
            }
            else if (!strncmp(mnem,"db",2)) {
                if (tgt!=0xFFFFFFFF) push(tgt);   /* dbcc: target + fall through */
            }
            if (stop_fallthru) break;
            pc += len;
        }
    }
}

int main(int argc,char**argv)
{
    if (argc<2){ fprintf(stderr,"usage: %s shinobi_main.bin\n",argv[0]); return 1; }
    FILE *f=fopen(argv[1],"rb"); if(!f){perror("fopen");return 1;}
    BUFSZ=fread(BUF,1,ROMSZ,f); fclose(f);

    /* seeds: reset PC, IRQ4 handler, and every distinct live vector target */
    push(0x000400);
    push(0x002684);
    for (int v=0; v<64; v++) {
        unsigned t=(BUF[v*4]<<24)|(BUF[v*4+1]<<16)|(BUF[v*4+2]<<8)|BUF[v*4+3];
        t &= 0xFFFFFF;
        if (t>=0x400 && t<BUFSZ) push(t);
    }

    /* Phase 1: strict recursive descent from real entry points */
    drain();
    long strict_instr = n_instr, strict_cov=0;
    for (unsigned a=0x400;a<BUFSZ;a++) if(covered[a]) strict_cov++;
    long strict_cand = cand_n;

    /* Phase 2: immediate-seeded discovery — push every harvested ROM-range
     * immediate (handler pointers / pea targets) and descend to fixpoint. */
    int last;
    do {
        last = n_instr;
        for (int i=0;i<cand_n;i++) push(cand[i]);
        drain();
    } while (n_instr != last);

    /* unreachable tails: covered=0 ROM bytes between min/max code */
    long uncov=0; for (unsigned a=0x400;a<BUFSZ;a++) if(!covered[a]) uncov++;

    /* ---- REPORT ---- */
    printf("==== Shinobi rebase-recompiler static analysis ====\n");
    printf("program image       : %u bytes\n", BUFSZ);
    printf("PHASE 1 strict recursive-descent (reset+IRQ4+vectors only):\n");
    printf("   reachable instrs  : %ld   bytes covered: %ld   (blocked by computed jumps)\n",
           strict_instr, strict_cov);
    printf("   harvested handler-pointer immediates: %ld\n", strict_cand);
    printf("PHASE 2 + immediate-seeded discovery (harvest #$romaddr handler ptrs):\n");
    printf("   reachable instrs  : %ld\n", n_instr);
    printf("bytes covered       : %ld / %u  (uncovered ROM bytes: %ld = data/sprite-list/unreached)\n",
           (long)BUFSZ-uncov, BUFSZ, uncov);
    printf("\n-- addressing forms (operand occurrences) --\n");
    printf("  abs.l ($xxxxxx.l) : %ld\n", n_abs_l);
    printf("  abs.w ($xxxx.w)   : %ld   (sign-extend to 0xFFxxxx workram / low pages)\n", n_abs_w);
    printf("  PC-relative       : %ld   (NO rebase needed)\n", n_pcrel);
    printf("  register-indirect : %ld   (An-based EA; target not statically known)\n", n_reg_ind);

    printf("\n-- absolute memory operands by target region --\n");
    for (int i=0;i<R_NREG;i++)
        if (region_hits[i])
            printf("  %-14s : %6ld operands   in %ld instrs   [%s]\n",
                   RNAME[i], region_hits[i], region_instr[i],
                   is_mmio(i)?"REWRITE->HAL":(i==R_ROM?"rebase":"rebase (RAM)"));

    printf("\n-- MMIO-touching instructions: %ld total --\n", mmio_instr);
    printf("   unique instruction forms that hit MMIO (all are absolute EAs):\n");
    for (int i=0;i<mforms_n;i++)
        printf("     [%06x x%-4ld] %s\n", mforms[i].addr, mforms[i].count, mforms[i].text);

    printf("\n-- computed-jump / jump-table sites (cannot follow statically): %d --\n", cjmp_n);
    for (int i=0;i<cjmp_n && i<40;i++)
        printf("     [%06x] %s\n", cjmp[i], cjmp_txt[i]);
    if (cjmp_n>40) printf("     ... (%d more)\n", cjmp_n-40);

    return 0;
}
