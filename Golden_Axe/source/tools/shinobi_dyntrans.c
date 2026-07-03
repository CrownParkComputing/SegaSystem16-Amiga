/*
 * shinobi_dyntrans.c — HOST prototype of the Shinobi (Sega System 16B) DYNAMIC
 * BINARY TRANSLATOR (68000 -> 68020 rebase).
 *
 * WHY a dynamic translator: the A1200 target is a bare 68EC020 (no MMU).  The
 * game dispatches handlers through RAM vtables / computed jumps, so a STATIC
 * recursive descent reaches only ~4% of the code (see tools/shinobi_rebase.c).
 * The fix is ON-DEMAND block translation at runtime: discover guest PCs as the
 * RAM vtables resolve, keyed by guest PC in a translation cache.
 *
 * WHAT this host prototype proves:
 *   (1) a base-offset FLAT guest address space (guest G lives at host MEM[G]) +
 *       an I/O-SHADOW model is correct -> VRAM/text/palette/sprite come out
 *       BYTE-FOR-BYTE identical to the golden reference (shinobi_host.c);
 *   (2) the BLOCK-BOUNDARY + DISPATCH + on-demand-discovery machinery reaches
 *       the runtime code set the static pass cannot (the computed-jump targets);
 *   (3) the terminator-type breakdown -> how many DYNAMIC vtable dispatches.
 *
 * The per-block EXECUTOR for this prototype is the vendored Musashi (we set its
 * PC to the guest PC and single-step one guest instruction at a time, observing
 * the runtime next-PC after each control-flow boundary).  On the Amiga the
 * translated block becomes real rebased 020 code instead (see the design notes
 * printed at the end / shinobi-port-facts.md).
 *
 * It runs the SAME System-16B map + 60Hz IRQ4(->0x2684) model as shinobi_host.c
 * but routes execution through the dispatch loop + translation cache, then
 * compares the resulting RAM regions against a reference run done in the same
 * process.
 *
 * Build:
 *   cc -O2 -I src/cores/m68k -I src/cores/m68k/softfloat -o /tmp/shinobi_dyntrans \
 *       tools/shinobi_dyntrans.c \
 *       src/cores/m68k/m68kcpu.c src/cores/m68k/m68kops.c src/cores/m68k/m68kdasm.c \
 *       src/cores/m68k/softfloat/softfloat.c -lm
 * Run:
 *   /tmp/shinobi_dyntrans games/shinobi/roms/shinobi_main.bin [frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "m68k.h"

/* ====================================================================== */
/* FLAT GUEST ADDRESS SPACE: guest G (24-bit) lives at host MEM[G].         */
/* On the Amiga this becomes base+G; here base=0 so MEM[] IS the space.     */
/* RAM/VRAM/workram/palette/sprite + the I/O-shadow page all live here.     */
/* ====================================================================== */
#define GUEST_SIZE 0x1000000
static uint8_t MEM[GUEST_SIZE];
static unsigned ROMSZ = 0;

/* ---- I/O-shadow + MMIO model (identical semantics to shinobi_host.c) ----
 * The recommended port model treats I/O like VRAM: a shadow page the HAL
 * serves reads from (live inputs/DSW pre-frame) and scans for writes
 * (sound-latch / mapper-cmd) post-frame.  For the no-input reference this is
 * byte-exact with the golden harness, which is what we validate against. */
static uint8_t in_service = 0xff, in_p1 = 0xff, in_p2 = 0xff;
static uint8_t dsw1 = 0xff, dsw2 = 0xff;

static long g_irq4_raised, g_irq4_serviced, g_mapper_writes;
static long g_watchdog_kicks, g_sound_latch_writes, g_cmd_window_writes;
static long g_misc_out_writes, g_io_reads;
static uint8_t g_mapper_table[16]; static int g_mapper_have;
static uint8_t last_sound_latch;

static void io_reset_stats(void){
    g_irq4_raised=g_irq4_serviced=g_mapper_writes=0;
    g_watchdog_kicks=g_sound_latch_writes=g_cmd_window_writes=0;
    g_misc_out_writes=g_io_reads=0;
    memset(g_mapper_table,0,sizeof g_mapper_table); g_mapper_have=0;
    last_sound_latch=0;
}

static unsigned io_read(unsigned addr,int size){
    g_io_reads++;
    unsigned word_off=((addr-0xC40000)>>1)&0x1fff, val=0xffff;
    switch (word_off & (0x3000/2)) {
        case 0x1000/2:
            switch (word_off & 3){
                case 0: val=in_service; break;
                case 1: val=in_p1; break;
                case 2: val=0xff; break;
                case 3: val=in_p2; break;
            } break;
        case 0x2000/2: val=(word_off&1)?dsw1:dsw2; break;
        default: val=0xffff; break;
    }
    if (size==1) return val&0xff;
    return 0xff00|(val&0xff);
}
static void io_write(unsigned addr,unsigned val,int size){
    (void)size;
    if (addr==0xC40001 || (addr&~1)==0xC40000){ g_misc_out_writes++; return; }
    if ((addr&~1)==0xC43000){ g_sound_latch_writes++; last_sound_latch=val&0xff; return; }
    if ((addr&~7)==0xC43000){ return; }
}

/* region table — RAM/VRAM regions index into the FLAT MEM[] (base+G). */
static uint8_t *region_ptr(unsigned addr){
    addr &= 0xFFFFFF;
    if (addr < 0x040000)                      return MEM+addr;          /* ROM */
    if (addr >= 0x400000 && addr < 0x410000)  return MEM+addr;          /* tileram */
    if (addr >= 0x410000 && addr < 0x411000)  return MEM+addr;          /* textram */
    if (addr >= 0x440000 && addr < 0x440800)  return MEM+addr;          /* spriteram */
    if (addr >= 0x840000 && addr < 0x841000)  return MEM+addr;          /* palette */
    if (addr >= 0xFF0000)                      return MEM+addr;          /* work RAM */
    return NULL;
}

static unsigned read_bytes(unsigned addr,int size){
    addr &= 0xFFFFFF;
    if (addr>=0xC40000 && addr<0xC44000) return io_read(addr,size);
    if (addr>=0xC60000 && addr<0xC60002){ g_watchdog_kicks++; return 0xffff; }
    if (addr>=0x3F0000 && addr<0x400000)  return 0xffff;          /* cmd window open bus */
    if (addr>=0xFE0000 && addr<0xFE0040)  return 0xffff;          /* mapper regs */
    uint8_t *p=region_ptr(addr);
    if (!p) return (size==1)?0xff:(size==2)?0xffff:0xffffffff;
    unsigned v=0; for(int i=0;i<size;i++) v=(v<<8)|p[i]; return v;
}
static void write_bytes(unsigned addr,unsigned val,int size){
    addr &= 0xFFFFFF;
    if (addr>=0xC40000 && addr<0xC44000){ io_write(addr,val,size); return; }
    if (addr>=0xC60000 && addr<0xC60002){ g_watchdog_kicks++; return; }
    if (addr>=0x3F0000 && addr<0x400000){ g_cmd_window_writes++; return; }
    if (addr>=0xFE0000 && addr<0xFE0040){
        g_mapper_writes++;
        if (addr>=0xFE0020 && addr<0xFE0040){
            unsigned idx=(addr-0xFE0020)>>1;
            if (idx<16){ g_mapper_table[idx]=val&0xff; if(idx==15) g_mapper_have=1; }
        }
        return;
    }
    uint8_t *p=region_ptr(addr);
    if (!p) return;
    for (int i=size-1;i>=0;i--){ p[i]=val&0xff; val>>=8; }
}

/* Musashi memory callbacks */
unsigned int m68k_read_memory_8 (unsigned int a){ return read_bytes(a,1); }
unsigned int m68k_read_memory_16(unsigned int a){ return read_bytes(a,2); }
unsigned int m68k_read_memory_32(unsigned int a){ return read_bytes(a,4); }
void m68k_write_memory_8 (unsigned int a,unsigned int v){ write_bytes(a,v,1); }
void m68k_write_memory_16(unsigned int a,unsigned int v){ write_bytes(a,v,2); }
void m68k_write_memory_32(unsigned int a,unsigned int v){ write_bytes(a,v,4); }
unsigned int m68k_read_disassembler_8 (unsigned int a){ return read_bytes(a,1); }
unsigned int m68k_read_disassembler_16(unsigned int a){ return read_bytes(a,2); }
unsigned int m68k_read_disassembler_32(unsigned int a){ return read_bytes(a,4); }

static int int_ack(int level){ (void)level; m68k_set_irq(0); return M68K_INT_ACK_AUTOVECTOR; }

/* ====================================================================== */
/* TRANSLATION CACHE + BLOCK CLASSIFIER                                     */
/* ====================================================================== */
enum Term {
    T_NONE=0,   /* not a terminator (ordinary instruction)               */
    T_BRA, T_BCC, T_BSR, T_DBCC,        /* STATIC relative targets        */
    T_JMP_ABS, T_JSR_ABS,               /* STATIC absolute targets        */
    T_JMP_PCREL, T_JSR_PCREL,           /* computed PC-relative jump table*/
    T_JMP_IND, T_JSR_IND,               /* DYNAMIC vtable (jmp/jsr (An))   */
    T_RTS, T_RTR, T_RTE,                /* DYNAMIC return (target on stack)*/
    T_STOP,                              /* stop/reset/trap/illegal        */
    T_FALL,                              /* block hit size cap, falls thru */
    T_UNCL,                              /* could not decode               */
    T_N
};
static const char *TNAME[T_N]={
    "none","bra","bcc","bsr","dbcc","jmp.abs","jsr.abs","jmp(PC,Xn)","jsr(PC,Xn)",
    "jmp(An)","jsr(An)","rts","rtr","rte","stop/trap","fallthru","UNCLASSIFIED"
};
static int term_is_dynamic(int t){
    return t==T_JMP_IND||t==T_JSR_IND||t==T_RTS||t==T_RTR||t==T_RTE||
           t==T_JMP_PCREL||t==T_JSR_PCREL||t==T_STOP;
}
static int term_is_vtable(int t){ return t==T_JMP_IND||t==T_JSR_IND; }

static int is_hexc(char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}
static unsigned first_target(const char *dis){
    /* first '$' token not preceded by '#' or '(' (a branch/jump displacement) */
    const char *p=strchr(dis,'$');
    if (!p) return 0xFFFFFFFF;
    if (p>dis && (p[-1]=='#'||p[-1]=='(')) {
        /* try the next one only if it's a plain target (rare) */
        return 0xFFFFFFFF;
    }
    const char *q=p+1; unsigned v=0; int n=0;
    while(n<8 && is_hexc(*q)){ char c=*q++; v=(v<<4)|(c<='9'?c-'0':c-'a'+10); n++; }
    return n?v:0xFFFFFFFF;
}
static int is_branch_cc(const char *m){
    static const char *cc[]={"hi","ls","cc","cs","ne","eq","vc","vs",
                             "pl","mi","ge","lt","gt","le","hs","lo",0};
    for(int i=0;cc[i];i++) if(m[1]==cc[i][0]&&m[2]==cc[i][1]) return 1;
    return 0;
}

/* classify the instruction at guest pc; *plen = its byte length, *pstgt = the
 * static chain target where statically known. returns the Term. */
static int classify(unsigned pc, unsigned *plen, unsigned *pstgt){
    char dis[256];
    unsigned len=m68k_disassemble(dis,pc,M68K_CPU_TYPE_68000);
    *plen=len; *pstgt=0xFFFFFFFF;
    if (len==0||len>16) { *plen=2; return T_UNCL; }
    char m[24]={0}; sscanf(dis,"%23s",m);

    if (!strcmp(m,"rts"))  return T_RTS;
    if (!strcmp(m,"rtr"))  return T_RTR;
    if (!strcmp(m,"rte"))  return T_RTE;
    if (!strcmp(m,"reset")||!strcmp(m,"stop")||!strcmp(m,"illegal")||
        !strncmp(m,"trap",4)) return T_STOP;
    if (!strncmp(m,"bra",3)){ *pstgt=first_target(dis); return T_BRA; }
    if (!strncmp(m,"bsr",3)){ *pstgt=first_target(dis); return T_BSR; }
    if (!strncmp(m,"db",2)) { *pstgt=first_target(dis); return T_DBCC; }
    if (!strcmp(m,"jmp")){
        if (strchr(dis,'(')){
            if (strstr(dis,"PC")) return T_JMP_PCREL;
            return T_JMP_IND;
        }
        *pstgt=first_target(dis); return T_JMP_ABS;
    }
    if (!strcmp(m,"jsr")){
        if (strchr(dis,'(')){
            if (strstr(dis,"PC")) return T_JSR_PCREL;
            return T_JSR_IND;
        }
        *pstgt=first_target(dis); return T_JSR_ABS;
    }
    if (m[0]=='b' && strlen(m)>=3 && is_branch_cc(m)){
        *pstgt=first_target(dis); return T_BCC;
    }
    return T_NONE;
}

/* ---- block cache ---- */
#define BCAP 512                 /* max instrs walked per block            */
typedef struct {
    unsigned pc;                 /* block head (key)                        */
    int      n;                  /* instruction count                       */
    int      term;               /* terminator Term                         */
    unsigned stgt;               /* static chain target (if known)          */
    unsigned iaddr[BCAP];        /* instruction start addresses             */
    uint8_t  ilen[BCAP];         /* instruction byte lengths                */
    long     exec_count;         /* times this block was entered            */
    int      reached_dyn;        /* head was a destination of a dynamic edge*/
} Block;

#define HASHN (1<<17)
static int   htab[HASHN];        /* -1 empty else index into blocks[]       */
static Block blocks[40000];
static int   nblocks=0;
static int   over_blocks=0;      /* blocks beyond array capacity            */

static unsigned hashpc(unsigned pc){ return ((pc*2654435761u)>>11) & (HASHN-1); }

static Block *cache_get(unsigned pc){
    unsigned h=hashpc(pc);
    while (htab[h]!=-1){ if(blocks[htab[h]].pc==pc) return &blocks[htab[h]]; h=(h+1)&(HASHN-1);}
    return NULL;
}

/* on-demand TRANSLATE: walk from pc to the first control-flow boundary,
 * recording instruction boundaries + the terminator classification. */
static long n_unclassified_sites=0;
static unsigned unclassified_pcs[64]; static int n_unclassified_pcs=0;

static Block *translate(unsigned pc){
    if (nblocks>=(int)(sizeof blocks/sizeof blocks[0])){ over_blocks++; return NULL; }
    Block *b=&blocks[nblocks];
    b->pc=pc; b->n=0; b->stgt=0xFFFFFFFF; b->exec_count=0; b->reached_dyn=0;
    unsigned a=pc;
    for(;;){
        unsigned len,stgt; int t=classify(a,&len,&stgt);
        if (b->n<BCAP){ b->iaddr[b->n]=a; b->ilen[b->n]=(uint8_t)len; }
        b->n++;
        if (t==T_UNCL){
            b->term=T_UNCL;
            n_unclassified_sites++;
            if (n_unclassified_pcs<64) unclassified_pcs[n_unclassified_pcs++]=a;
            break;
        }
        if (t!=T_NONE){ b->term=t; b->stgt=stgt; break; }
        a+=len;
        if (b->n>=BCAP){ b->term=T_FALL; break; }
    }
    /* install */
    unsigned h=hashpc(pc);
    while (htab[h]!=-1) h=(h+1)&(HASHN-1);
    htab[h]=nblocks;
    nblocks++;
    return b;
}
static Block *lookup_or_translate(unsigned pc){
    Block *b=cache_get(pc); if(b) return b; return translate(pc);
}

/* ---- dynamic-dispatch edge tracking (vtable target resolution) ---- */
#define ECAP (1<<16)
static struct { unsigned src,dst; } edges[ECAP]; static int n_edges=0;
static long n_dyn_dispatch=0;      /* executions ending in a dynamic term   */
static long n_vtable_dispatch=0;   /* executions of jmp/jsr (An)            */
static void record_edge(unsigned src,unsigned dst){
    n_dyn_dispatch++;
    /* dedup distinct (src,dst) */
    unsigned h=((src*2654435761u ^ dst*40503u)>>9)&(ECAP-1);
    while (edges[h].src||edges[h].dst){
        if (edges[h].src==src && edges[h].dst==dst) return;
        h=(h+1)&(ECAP-1);
    }
    if (n_edges<ECAP-1){ edges[h].src=src; edges[h].dst=dst; n_edges++; }
}

/* ---- runtime coverage bitmaps over the ROM range ---- */
static uint8_t exec_bm[0x40000];     /* instruction-START addresses executed */
static uint8_t covered_bm[0x40000];  /* every byte belonging to an exec instr */
static long exec_distinct=0, bytes_covered=0;
static void mark_exec(unsigned pc,unsigned len){
    if (pc<0x40000 && !exec_bm[pc]){ exec_bm[pc]=1; exec_distinct++; }
    for (unsigned i=0;i<len && pc+i<0x40000;i++)
        if (!covered_bm[pc+i]){ covered_bm[pc+i]=1; bytes_covered++; }
}

/* ====================================================================== */
/* INSTRUCTION HOOK — records the PC of the single guest instruction that    */
/* each m68k_execute(1) retires, so the dispatcher can detect an IRQ-induced  */
/* control transfer (handler entry) that happened *inside* a step.           */
/* ====================================================================== */
static volatile unsigned g_last_exec_pc=0xFFFFFFFF;
static int g_dyn_mode=0;
static void instr_hook(unsigned int pc){
    if (pc==0x2684) g_irq4_serviced++;
    if (g_dyn_mode) g_last_exec_pc=pc;
}

/* register a block head discovered at runtime (e.g. an IRQ handler entry). */
static void register_head(unsigned pc){ lookup_or_translate(pc); }

/* ====================================================================== */
/* THE DISPATCH LOOP.  Keyed by guest PC, backed by the translation cache.   */
/* Each block is "executed" by single-stepping Musashi through its instrs    */
/* until the control-flow boundary; the runtime next-PC is read back from    */
/* Musashi (the resolved vtable / rts / branch target).                      */
/* ====================================================================== */
static long step_one(void){
    int c=m68k_execute(1);
    return c>0?c:1;
}

/* execute one cached block; returns guest cycles consumed via *pcyc, and the
 * runtime-resolved next guest PC as the function result. */
static unsigned exec_block(Block *b, long *pcyc){
    b->exec_count++;
    long cyc=0;
    int n = b->n<BCAP ? b->n : BCAP;
    for (int i=0;i<n;i++){
        unsigned exp=b->iaddr[i];
        unsigned cur=m68k_get_reg(NULL,M68K_REG_PC);
        if (cur!=exp){
            /* control already diverted before this instr (shouldn't normally
             * happen because IRQs are serviced inside the step) — resync. */
            *pcyc=cyc; return cur;
        }
        g_last_exec_pc=0xFFFFFFFF;
        cyc += step_one();
        if (g_last_exec_pc!=exp){
            /* an IRQ/exception was injected at the top of this step: the
             * instruction that actually retired (g_last_exec_pc) is a NEW
             * dispatch target (the handler entry).  Discover it + resync. */
            if (g_last_exec_pc!=0xFFFFFFFF) register_head(g_last_exec_pc);
            *pcyc=cyc; return m68k_get_reg(NULL,M68K_REG_PC);
        }
        mark_exec(exp,b->ilen[i]);
    }
    unsigned next=m68k_get_reg(NULL,M68K_REG_PC);
    if (term_is_dynamic(b->term)){
        record_edge(b->pc,next);
        Block *d=cache_get(next); if(d) d->reached_dyn=1;  /* head reached dynamically */
    }
    if (term_is_vtable(b->term))  n_vtable_dispatch++;
    *pcyc=cyc; return next;
}

/* ====================================================================== */
/* MACHINE INIT / RESET                                                     */
/* ====================================================================== */
static unsigned char ROMIMG[0x40000]; static size_t ROMN=0;
static void mem_init(void){
    memset(MEM,0,GUEST_SIZE);
    memcpy(MEM,ROMIMG,ROMN);          /* program ROM at guest 0 */
}

/* snapshot regions for comparison */
typedef struct { uint8_t tile[0x10000],text[0x1000],spr[0x800],pal[0x1000],work[0x10000]; } Snap;
static void snap_take(Snap *s){
    memcpy(s->tile,MEM+0x400000,0x10000);
    memcpy(s->text,MEM+0x410000,0x1000);
    memcpy(s->spr ,MEM+0x440000,0x800);
    memcpy(s->pal ,MEM+0x840000,0x1000);
    memcpy(s->work,MEM+0xFF0000,0x10000);
}

static const int CYC_PER_FRAME = 10000000/60;

/* ---- reference run: plain Musashi frame loop (identical to shinobi_host) -- */
static void run_reference(int frames){
    g_dyn_mode=0; io_reset_stats(); mem_init();
    m68k_pulse_reset();
    for (int fr=0;fr<frames;fr++){
        m68k_set_irq(4); g_irq4_raised++;
        int rem=CYC_PER_FRAME;
        while (rem>0) rem -= m68k_execute(rem>20000?20000:rem);
        m68k_set_irq(0);
    }
}

/* ---- dynamic run: dispatch loop + translation cache ---- */
static long dyn_total_dispatches=0;
/* ordered block-PC trace (deduped consecutive, first N) for the differential
 * first-divergence diff against the on-target translator's frozen boot ring. */
static unsigned dbg_trace[1023]; static int dbg_trace_n=0; static unsigned dbg_trace_last=0xFFFFFFFF;
static void run_dynamic(int frames){
    g_dyn_mode=1; io_reset_stats(); mem_init();
    m68k_pulse_reset();
    for (int fr=0;fr<frames;fr++){
        m68k_set_irq(4); g_irq4_raised++;
        long budget=CYC_PER_FRAME;
        while (budget>0){
            unsigned pc=m68k_get_reg(NULL,M68K_REG_PC);
            if (pc!=dbg_trace_last && dbg_trace_n<1023){ dbg_trace[dbg_trace_n++]=pc; dbg_trace_last=pc; }
            else if (pc!=dbg_trace_last) dbg_trace_last=pc;
            Block *b=lookup_or_translate(pc);
            if (!b){ /* cache full: fall back to a raw step to keep going */
                budget-=step_one(); continue;
            }
            long cyc=0;
            (void)exec_block(b,&cyc);
            dyn_total_dispatches++;
            budget-=cyc;
        }
        m68k_set_irq(0);
    }
}

/* ====================================================================== */
/* REPORTING                                                                */
/* ====================================================================== */
static int region_nz(const uint8_t*p,int len){ int n=0; for(int i=0;i<len;i+=2) if(p[i]||p[i+1])n++; return n; }

static void print_boot_stats(const char*tag){
    fprintf(stderr,"[%s] IRQ4 raised/served=%ld/%ld  mapper_writes=%ld table=",
            tag,g_irq4_raised,g_irq4_serviced,g_mapper_writes);
    for(int i=0;i<16;i++) fprintf(stderr,"%02x",g_mapper_table[i]);
    fprintf(stderr,"\n[%s] sndlatch=%ld(last=%02x) cmdwin=%ld misc_out=%ld io_reads=%ld watchdog=%ld\n",
            tag,g_sound_latch_writes,last_sound_latch,g_cmd_window_writes,
            g_misc_out_writes,g_io_reads,g_watchdog_kicks);
}

int main(int argc,char**argv){
    if (argc<2){ fprintf(stderr,"usage: %s shinobi_main.bin [frames]\n",argv[0]); return 1; }
    int frames=(argc>=3)?atoi(argv[2]):120;
    FILE *f=fopen(argv[1],"rb"); if(!f){perror("fopen");return 1;}
    ROMN=fread(ROMIMG,1,sizeof ROMIMG,f); fclose(f);
    ROMSZ=(unsigned)ROMN;
    fprintf(stderr,"loaded %zu bytes program ROM; running %d frames\n",ROMN,frames);

    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_init();
    m68k_set_int_ack_callback(int_ack);
    m68k_set_instr_hook_callback(instr_hook);

    for(int i=0;i<HASHN;i++) htab[i]=-1;

    /* 1) GOLDEN REFERENCE run (plain frame loop) */
    run_reference(frames);
    fprintf(stderr,"\n=== REFERENCE (plain Musashi) ===\n");
    print_boot_stats("ref");
    Snap ref; snap_take(&ref);
    fprintf(stderr,"[ref] nz: tile=%d text=%d spr=%d pal=%d\n",
            region_nz(ref.tile,0x10000),region_nz(ref.text,0x1000),
            region_nz(ref.spr,0x800),region_nz(ref.pal,0x1000));

    /* 2) DYNAMIC-DISPATCH run (translation cache + block dispatch) */
    run_dynamic(frames);
    fprintf(stderr,"\n=== DYNAMIC TRANSLATOR (dispatch + on-demand discovery) ===\n");
    print_boot_stats("dyn");
    Snap dyn; snap_take(&dyn);
    fprintf(stderr,"[dyn] nz: tile=%d text=%d spr=%d pal=%d\n",
            region_nz(dyn.tile,0x10000),region_nz(dyn.text,0x1000),
            region_nz(dyn.spr,0x800),region_nz(dyn.pal,0x1000));

    /* 3) BYTE-FOR-BYTE comparison of the rendered regions */
    int dT=memcmp(ref.tile,dyn.tile,0x10000);
    int dX=memcmp(ref.text,dyn.text,0x1000);
    int dS=memcmp(ref.spr ,dyn.spr ,0x800);
    int dP=memcmp(ref.pal ,dyn.pal ,0x1000);
    int dW=memcmp(ref.work,dyn.work,0x10000);
    fprintf(stderr,"\n=== REBASE/I-O-SHADOW VALIDATION (dynamic vs reference) ===\n");
    fprintf(stderr,"  tileram   : %s\n", dT?"*** DIFFER ***":"identical");
    fprintf(stderr,"  textram   : %s\n", dX?"*** DIFFER ***":"identical");
    fprintf(stderr,"  spriteram : %s\n", dS?"*** DIFFER ***":"identical");
    fprintf(stderr,"  palette   : %s\n", dP?"*** DIFFER ***":"identical");
    fprintf(stderr,"  work RAM  : %s\n", dW?"*** DIFFER ***":"identical");
    int allok = !(dT||dX||dS||dP||dW);
    fprintf(stderr,"  VERDICT   : %s\n",
            allok?"BYTE-EXACT MATCH over all frames":"MISMATCH");

    /* 4) DYNAMIC DISCOVERY report */
    /* complete the reached-dynamically flags from the deduped edge set */
    for (int h=0;h<ECAP;h++) if (edges[h].src||edges[h].dst){
        Block *d=cache_get(edges[h].dst); if(d) d->reached_dyn=1;
    }
    long blk_dynonly=0; for(int i=0;i<nblocks;i++) if(blocks[i].reached_dyn) blk_dynonly++;
    /* distinct dynamic-dispatch destination addresses */
    long distinct_dyn_dst=0; { static uint8_t seen[0x1000000];
        for(int h=0;h<ECAP;h++) if((edges[h].src||edges[h].dst)&&edges[h].dst<0x1000000)
            if(!seen[edges[h].dst]){ seen[edges[h].dst]=1; distinct_dyn_dst++; } }
    fprintf(stderr,"\n=== DYNAMIC CODE DISCOVERY (runtime-reached set) ===\n");
    fprintf(stderr,"  distinct blocks discovered (cache size) : %d%s\n",
            nblocks, over_blocks?" (+overflow!)":"");
    fprintf(stderr,"  ...of which reached via a DYNAMIC edge  : %ld  (heads static descent cannot follow)\n",
            blk_dynonly);
    fprintf(stderr,"  distinct dynamic-dispatch targets resolved: %ld\n",distinct_dyn_dst);
    fprintf(stderr,"  total block dispatches executed         : %ld\n",dyn_total_dispatches);
    fprintf(stderr,"  distinct guest instruction PCs executed : %ld\n",exec_distinct);
    fprintf(stderr,"  ROM bytes covered at runtime            : %ld / %u  (%.1f%%)\n",
            bytes_covered, ROMSZ, 100.0*bytes_covered/ROMSZ);
    fprintf(stderr,"  NB: 120 frames of boot+attract is a small working set; the point is that the\n");
    fprintf(stderr,"      dispatch loop reaches the RUNTIME vtable/jump targets the STATIC descent\n");
    fprintf(stderr,"      stops dead at (static strict descent = ~3098 instrs / 10886 bytes, ~4%%,\n");
    fprintf(stderr,"      blocked precisely at these computed jumps).\n");

    /* 5) TERMINATOR-TYPE breakdown */
    long tcount[T_N]; long texec[T_N];
    memset(tcount,0,sizeof tcount); memset(texec,0,sizeof texec);
    for (int i=0;i<nblocks;i++){ tcount[blocks[i].term]++; texec[blocks[i].term]+=blocks[i].exec_count; }
    fprintf(stderr,"\n=== BLOCK TERMINATOR BREAKDOWN ===\n");
    fprintf(stderr,"  %-14s %8s %12s\n","terminator","#blocks","#executions");
    for (int t=1;t<T_N;t++) if (tcount[t]||texec[t])
        fprintf(stderr,"  %-14s %8ld %12ld%s\n",TNAME[t],tcount[t],texec[t],
                term_is_vtable(t)?"   <-- RAM vtable dispatch":
                (term_is_dynamic(t)?"   (dynamic)":""));
    long dyn_blocks=0,vt_blocks=0;
    for(int t=1;t<T_N;t++){ if(term_is_dynamic(t))dyn_blocks+=tcount[t]; if(term_is_vtable(t))vt_blocks+=tcount[t]; }
    fprintf(stderr,"  -> dynamic-terminator blocks   : %ld\n",dyn_blocks);
    fprintf(stderr,"  -> RAM-vtable jmp/jsr(An) blocks: %ld (executed %ld times)\n",
            vt_blocks,n_vtable_dispatch);
    fprintf(stderr,"  -> dynamic dispatches executed : %ld (distinct src->dst edges: %d)\n",
            n_dyn_dispatch,n_edges);

    /* 6) unclassified sites */
    fprintf(stderr,"\n=== UNCLASSIFIED / UNDECODED SITES ===\n");
    fprintf(stderr,"  count: %ld\n",n_unclassified_sites);
    for (int i=0;i<n_unclassified_pcs;i++) fprintf(stderr,"   %06x\n",unclassified_pcs[i]);

    /* optional: dump the runtime-reached block-head PC bitmap (1 bit / ROM byte) for a
     * differential coverage diff against the on-target translator's ring/coverage. */
    { const char *cd=getenv("SHINOBI_COVDUMP");
      if(cd){ static uint8_t bm[0x40000/8]={0};
        for(int i=0;i<nblocks;i++){ unsigned p=blocks[i].pc; if(p<0x40000) bm[p>>3]|=(uint8_t)(1u<<(p&7)); }
        FILE*cf=fopen(cd,"wb"); if(cf){ fwrite(bm,1,sizeof bm,cf); fclose(cf);
          fprintf(stderr,"[covdump] %d block heads -> %s\n",nblocks,cd);} } }

    { const char *td=getenv("SHINOBI_TRACEDUMP");
      if(td){ FILE*tf=fopen(td,"w"); if(tf){ for(int i=0;i<dbg_trace_n;i++) fprintf(tf,"%05x\n",dbg_trace[i]); fclose(tf);
        fprintf(stderr,"[tracedump] %d ordered block PCs -> %s\n",dbg_trace_n,td);} } }

    fprintf(stderr,"\nfinal PC=%06x\n",m68k_get_reg(NULL,M68K_REG_PC));
    return allok?0:2;
}
