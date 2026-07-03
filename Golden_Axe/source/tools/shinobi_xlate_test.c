/*
 * shinobi_xlate_test.c — HOST validator for the shared translation core
 * (tools/shinobi_xlate.c).  This is the oracle for the Amiga emitter: it runs
 * the SAME dynamic dispatch as the proven prototype (Musashi as executor, which
 * reproduces the golden VRAM byte-exact), and at every block it discovers it
 * cross-checks shinobi_xlate's hand-written 68000 decoder against Musashi's
 * m68kdasm:
 *     - instruction byte LENGTH must match m68k_disassemble's length,
 *     - terminator CLASS must match the prototype's Musashi-mnemonic classify,
 *     - the static branch/jump TARGET must match.
 * If the decoder agrees over the entire runtime-reachable instruction set, the
 * Amiga runtime — which uses the identical decoder to emit rebased 020 code —
 * is decoding correctly.  It also calls the emitter on every block to confirm
 * the emit path is structurally sound (no overflow / bad slot offsets).
 *
 * Build:
 *   cc -O2 -I src/cores/m68k -I src/cores/m68k/softfloat -I tools \
 *      -o /tmp/shinobi_xlate_test tools/shinobi_xlate_test.c tools/shinobi_xlate.c \
 *      src/cores/m68k/m68kcpu.c src/cores/m68k/m68kops.c src/cores/m68k/m68kdasm.c \
 *      src/cores/m68k/softfloat/softfloat.c -lm
 * Run: /tmp/shinobi_xlate_test games/shinobi/roms/shinobi_main.bin [frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "m68k.h"
#include "shinobi_xlate.h"

#define GUEST_SIZE 0x1000000
static uint8_t MEM[GUEST_SIZE];
static unsigned char ROMIMG[0x40000]; static size_t ROMN=0;

/* ---- same flat map + I/O-shadow model as the prototype ---- */
static uint8_t in_service=0xff,in_p1=0xff,in_p2=0xff,dsw1=0xff,dsw2=0xff;
static unsigned io_read(unsigned addr,int size){
    unsigned word_off=((addr-0xC40000)>>1)&0x1fff,val=0xffff;
    switch (word_off & (0x3000/2)){
        case 0x1000/2: switch(word_off&3){case 0:val=in_service;break;case 1:val=in_p1;break;
                       case 2:val=0xff;break;case 3:val=in_p2;break;} break;
        case 0x2000/2: val=(word_off&1)?dsw1:dsw2; break;
        default: val=0xffff; break;
    }
    return size==1?(val&0xff):(0xff00|(val&0xff));
}
static uint8_t *region_ptr(unsigned a){
    a&=0xFFFFFF;
    if(a<0x040000)return MEM+a;
    if(a>=0x400000&&a<0x410000)return MEM+a;
    if(a>=0x410000&&a<0x411000)return MEM+a;
    if(a>=0x440000&&a<0x440800)return MEM+a;
    if(a>=0x840000&&a<0x841000)return MEM+a;
    if(a>=0xFF0000)return MEM+a;
    return NULL;
}
static unsigned read_bytes(unsigned a,int size){
    a&=0xFFFFFF;
    if(a>=0xC40000&&a<0xC44000)return io_read(a,size);
    if(a>=0xC60000&&a<0xC60002)return 0xffff;
    if(a>=0x3F0000&&a<0x400000)return 0xffff;
    if(a>=0xFE0000&&a<0xFE0040)return 0xffff;
    uint8_t*p=region_ptr(a);
    if(!p)return size==1?0xff:size==2?0xffff:0xffffffff;
    unsigned v=0;for(int i=0;i<size;i++)v=(v<<8)|p[i];return v;
}
static void write_bytes(unsigned a,unsigned v,int size){
    a&=0xFFFFFF;
    if(a>=0xC40000&&a<0xC44000)return;
    if(a>=0xC60000&&a<0xC60002)return;
    if(a>=0x3F0000&&a<0x400000)return;
    if(a>=0xFE0000&&a<0xFE0040)return;
    uint8_t*p=region_ptr(a);if(!p)return;
    for(int i=size-1;i>=0;i--){p[i]=v&0xff;v>>=8;}
}
unsigned int m68k_read_memory_8 (unsigned int a){return read_bytes(a,1);}
unsigned int m68k_read_memory_16(unsigned int a){return read_bytes(a,2);}
unsigned int m68k_read_memory_32(unsigned int a){return read_bytes(a,4);}
void m68k_write_memory_8 (unsigned int a,unsigned int v){write_bytes(a,v,1);}
void m68k_write_memory_16(unsigned int a,unsigned int v){write_bytes(a,v,2);}
void m68k_write_memory_32(unsigned int a,unsigned int v){write_bytes(a,v,4);}
unsigned int m68k_read_disassembler_8 (unsigned int a){return read_bytes(a,1);}
unsigned int m68k_read_disassembler_16(unsigned int a){return read_bytes(a,2);}
unsigned int m68k_read_disassembler_32(unsigned int a){return read_bytes(a,4);}
static int int_ack(int l){(void)l;m68k_set_irq(0);return M68K_INT_ACK_AUTOVECTOR;}

/* ---- prototype's Musashi-mnemonic classifier (the reference for terminator) ---- */
static int is_hexc(char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}
static unsigned first_target(const char*dis){
    const char*p=strchr(dis,'$');if(!p)return 0xFFFFFFFF;
    if(p>dis&&(p[-1]=='#'||p[-1]=='('))return 0xFFFFFFFF;
    const char*q=p+1;unsigned v=0;int n=0;
    while(n<8&&is_hexc(*q)){char c=*q++;v=(v<<4)|(c<='9'?c-'0':c-'a'+10);n++;}
    return n?v:0xFFFFFFFF;
}
static int is_branch_cc(const char*m){
    static const char*cc[]={"hi","ls","cc","cs","ne","eq","vc","vs",
        "pl","mi","ge","lt","gt","le","hs","lo",0};
    for(int i=0;cc[i];i++)if(m[1]==cc[i][0]&&m[2]==cc[i][1])return 1;return 0;
}
static int ref_classify(unsigned pc,unsigned*plen,unsigned*pstgt){
    char dis[256];unsigned len=m68k_disassemble(dis,pc,M68K_CPU_TYPE_68000);
    *plen=len;*pstgt=0xFFFFFFFF;
    if(len==0||len>16){*plen=2;return XT_UNCL;}
    char m[24]={0};sscanf(dis,"%23s",m);
    if(!strcmp(m,"rts"))return XT_RTS;
    if(!strcmp(m,"rtr"))return XT_RTR;
    if(!strcmp(m,"rte"))return XT_RTE;
    if(!strcmp(m,"reset")||!strcmp(m,"stop")||!strcmp(m,"illegal")||!strncmp(m,"trap",4))return XT_STOP;
    if(!strncmp(m,"bra",3)){*pstgt=first_target(dis);return XT_BRA;}
    if(!strncmp(m,"bsr",3)){*pstgt=first_target(dis);return XT_BSR;}
    if(!strncmp(m,"db",2)){*pstgt=first_target(dis);return XT_DBCC;}
    if(!strcmp(m,"jmp")){if(strchr(dis,'(')){return strstr(dis,"PC")?XT_JMP_PCREL:XT_JMP_IND;}*pstgt=first_target(dis);return XT_JMP_ABS;}
    if(!strcmp(m,"jsr")){if(strchr(dis,'(')){return strstr(dis,"PC")?XT_JSR_PCREL:XT_JSR_IND;}*pstgt=first_target(dis);return XT_JSR_ABS;}
    if(m[0]=='b'&&strlen(m)>=3&&is_branch_cc(m)){*pstgt=first_target(dis);return XT_BCC;}
    return XT_NONE;
}

/* ---- cross-check stats ---- */
static long chk_total=0, chk_len_bad=0, chk_term_bad=0, chk_tgt_bad=0, chk_emit_bad=0;
static unsigned bad_pcs[64]; static int n_bad=0;
static uint8_t seen[0x40000];

static xl_emit_env EENV = { 0x01000000u, 0x00BEEF00u, 0x00CAFE00u,
                            0x00BEEF10u, 0x00BEEF20u, 0xDEADFA11u };  /* dummy on host */

static long audit_pcidx_data=0; static unsigned audit_pcidx_pcs[16]; static int n_audit=0;
static void crosscheck(unsigned pc){
    if(pc>=0x40000||seen[pc])return; seen[pc]=1;
    unsigned rlen,rtgt; int rterm=ref_classify(pc,&rlen,&rtgt);
    xdec d; xl_decode(MEM,GUEST_SIZE,pc,&d);
    /* AUDIT: a (d8,PC,Xn) brief-format DATA EA in a NON-terminator instruction is
     * copied verbatim by the emitter (note_abs ignores mode 7 reg 3), so after
     * relocation it reads the wrong guest address — a base-invisible bug.  Flag
     * any such site reached at runtime.  Musashi prints these as "...,PC,Xn...".*/
    { char dz[256]; m68k_disassemble(dz,pc,M68K_CPU_TYPE_68000);
      if(d.term==XT_NONE && strstr(dz,",PC,")){
          audit_pcidx_data++;
          if(n_audit<16) audit_pcidx_pcs[n_audit++]=pc; } }
    chk_total++;
    int bad=0;
    if(d.len!=rlen){ chk_len_bad++; bad=1; }
    if(d.term!=rterm){
        /* tolerate STOP-family disagreements only if both are "stop-ish" */
        chk_term_bad++; bad=1;
    }
    if((rterm==XT_BRA||rterm==XT_BCC||rterm==XT_BSR||rterm==XT_DBCC||
        rterm==XT_JMP_ABS||rterm==XT_JSR_ABS) && rtgt!=0xFFFFFFFF){
        if((d.stgt&0xFFFFFF)!=(rtgt&0xFFFFFF)){ chk_tgt_bad++; bad=1; }
    }
    /* exercise the emitter for structural soundness (non-terminator body) */
    if(d.term==XT_NONE){
        uint8_t buf[64]; int e=xl_emit_instr(MEM,GUEST_SIZE,pc,&d,&EENV,buf,sizeof buf);
        if(e<=0) { chk_emit_bad++; bad=1; }
        else {
            /* emitted body length = original length + 2 per widened slot
             * (abs.w and (d16,PC) both widen one 16-bit field to abs.l). */
            int nwide=0; for(int k=0;k<d.nabs;k++)
                if(d.abs[k].kind==XF_ABSW||d.abs[k].kind==XF_PCREL16||
                   d.abs[k].kind==XF_IMM_AN_W) nwide++;
            if(e != (int)d.len + 2*nwide){ chk_emit_bad++; bad=1; }
        }
    } else {
        uint8_t buf[64]; int e=xl_emit_term(pc,&d,pc+d.len,&EENV,buf,sizeof buf);
        /* e==0 is allowed for STOP/PCREL/complex-IND (runtime fault stub) */
        if(e<0) { chk_emit_bad++; bad=1; }
        (void)e;
    }
    if(bad && n_bad<64) bad_pcs[n_bad++]=pc;
}

/* ====================================================================== */
/* DISPATCH LOOP (mirrors the prototype; Musashi executor) — drives the    */
/* same reachable set, crosschecking each instruction as it is discovered. */
/* ====================================================================== */
#define BCAP 512
typedef struct { unsigned pc;int n;int term;unsigned ia[BCAP];uint8_t il[BCAP]; } Block;
#define HASHN (1<<17)
static int htab[HASHN]; static Block blocks[40000]; static int nblocks=0;
static unsigned hashpc(unsigned pc){return((pc*2654435761u)>>11)&(HASHN-1);}
static Block*cache_get(unsigned pc){unsigned h=hashpc(pc);
    while(htab[h]!=-1){if(blocks[htab[h]].pc==pc)return&blocks[htab[h]];h=(h+1)&(HASHN-1);}return NULL;}
static Block*translate(unsigned pc){
    if(nblocks>=40000)return NULL;
    Block*b=&blocks[nblocks];b->pc=pc;b->n=0;unsigned a=pc;
    for(;;){
        xdec d; xl_decode(MEM,GUEST_SIZE,a,&d);
        crosscheck(a);
        if(b->n<BCAP){b->ia[b->n]=a;b->il[b->n]=(uint8_t)d.len;}b->n++;
        if(d.term!=XT_NONE){b->term=d.term;break;}
        a+=d.len; if(b->n>=BCAP){b->term=XT_FALL;break;}
    }
    unsigned h=hashpc(pc);while(htab[h]!=-1)h=(h+1)&(HASHN-1);htab[h]=nblocks;nblocks++;return b;
}
static Block*lookup_or_translate(unsigned pc){Block*b=cache_get(pc);return b?b:translate(pc);}

static volatile unsigned g_last_pc=0xFFFFFFFF; static int g_dyn=0;
static void instr_hook(unsigned pc){ if(g_dyn)g_last_pc=pc; }
static long step_one(void){int c=m68k_execute(1);return c>0?c:1;}
static unsigned exec_block(Block*b,long*pc){
    long cyc=0;int n=b->n<BCAP?b->n:BCAP;
    for(int i=0;i<n;i++){
        unsigned exp=b->ia[i],cur=m68k_get_reg(NULL,M68K_REG_PC);
        if(cur!=exp){*pc=cyc;return cur;}
        g_last_pc=0xFFFFFFFF;cyc+=step_one();
        if(g_last_pc!=exp){if(g_last_pc!=0xFFFFFFFF)lookup_or_translate(g_last_pc);*pc=cyc;return m68k_get_reg(NULL,M68K_REG_PC);}
    }
    *pc=cyc;return m68k_get_reg(NULL,M68K_REG_PC);
}
static const int CYC_PER_FRAME=10000000/60;
static void mem_init(void){memset(MEM,0,GUEST_SIZE);memcpy(MEM,ROMIMG,ROMN);}
static void run_dynamic(int frames){
    g_dyn=1;mem_init();m68k_pulse_reset();
    for(int fr=0;fr<frames;fr++){
        m68k_set_irq(4);long budget=CYC_PER_FRAME;
        while(budget>0){
            unsigned pc=m68k_get_reg(NULL,M68K_REG_PC);
            Block*b=lookup_or_translate(pc);
            if(!b){budget-=step_one();continue;}
            long cyc=0;(void)exec_block(b,&cyc);budget-=cyc;
        }
        m68k_set_irq(0);
    }
}

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s shinobi_main.bin [frames]\n",argv[0]);return 1;}
    int frames=argc>=3?atoi(argv[2]):120;
    FILE*f=fopen(argv[1],"rb");if(!f){perror("fopen");return 1;}
    ROMN=fread(ROMIMG,1,sizeof ROMIMG,f);fclose(f);
    fprintf(stderr,"loaded %zu bytes; running %d frames\n",ROMN,frames);
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);m68k_init();
    m68k_set_int_ack_callback(int_ack);m68k_set_instr_hook_callback(instr_hook);
    for(int i=0;i<HASHN;i++)htab[i]=-1;
    run_dynamic(frames);

    fprintf(stderr,"\n=== SHARED DECODER vs MUSASHI (runtime-reachable set) ===\n");
    fprintf(stderr,"  distinct instructions checked : %ld\n",chk_total);
    fprintf(stderr,"  length mismatches             : %ld\n",chk_len_bad);
    fprintf(stderr,"  terminator-class mismatches   : %ld\n",chk_term_bad);
    fprintf(stderr,"  static-target mismatches      : %ld\n",chk_tgt_bad);
    fprintf(stderr,"  emitter structural failures   : %ld\n",chk_emit_bad);
    if(n_bad){
        fprintf(stderr,"  first divergent PCs:\n");
        for(int i=0;i<n_bad;i++){
            unsigned pc=bad_pcs[i],rl,rt;int rterm=ref_classify(pc,&rl,&rt);
            xdec d;xl_decode(MEM,GUEST_SIZE,pc,&d);
            char dis[256];m68k_disassemble(dis,pc,M68K_CPU_TYPE_68000);
            fprintf(stderr,"   %06x  w=%04x  '%s'  ref[len=%u term=%s tgt=%06x]  mine[len=%u term=%s tgt=%06x]\n",
                pc,d.w0,dis,rl,xt_name[rterm],rt&0xFFFFFF,d.len,xt_name[d.term],d.stgt&0xFFFFFF);
        }
    }
    fprintf(stderr,"\n=== AUDIT: un-rebased (d8,PC,Xn) DATA EAs on the reachable path ===\n");
    fprintf(stderr,"  count: %ld%s\n",audit_pcidx_data, audit_pcidx_data?" *** would mis-read after rebase ***":" (none -- emitter is complete for the path)");
    for(int i=0;i<n_audit;i++){ char dz[256]; m68k_disassemble(dz,audit_pcidx_pcs[i],M68K_CPU_TYPE_68000);
        fprintf(stderr,"   %06x  %s\n",audit_pcidx_pcs[i],dz); }

    long bad=chk_len_bad+chk_term_bad+chk_tgt_bad+chk_emit_bad;
    fprintf(stderr,"\n  VERDICT: %s  (%ld blocks discovered)\n",
            bad?"*** DECODER DIVERGES ***":"DECODER MATCHES MUSASHI over the reachable set",nblocks);
    return bad?2:0;
}
