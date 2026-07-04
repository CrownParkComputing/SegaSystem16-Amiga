/* tools/shinobi_shot.c -- render a Shinobi (Sega System 16B) attract/title frame
 * to a PPM, to VALIDATE the build-time gfx decode + tilemap/text/sprite/palette
 * logic in portable C, BEFORE the Amiga renderer. The Shinobi analogue of
 * tools/pacland_shot.c.
 *
 * It is SELF-CONTAINED: it embeds the same Musashi 68000 reference harness as
 * tools/shinobi_host.c (the golden oracle) -- post-mapper S16B map, array-backed
 * RAM, logging-stub MMIO, reset-vector seed, 60Hz IRQ4 -- runs N frames, then
 * renders the resulting VRAM (2 tilemaps + text + sprite list + palette) exactly
 * as segas16b_v.cpp / segaic16.cpp / sega16sp.cpp do in MAME, using the tile and
 * sprite gfx ROMs decoded inline (identical logic to tools/shinobi_decode_gfx.py).
 *
 * Build:
 *   cc -O2 -I src/cores/m68k -I src/cores/m68k/softfloat -o build/shinobi_shot \
 *       tools/shinobi_shot.c \
 *       src/cores/m68k/m68kcpu.c src/cores/m68k/m68kops.c src/cores/m68k/m68kdasm.c \
 *       src/cores/m68k/softfloat/softfloat.c -lm
 * Run:
 *   build/shinobi_shot [frames] [out.ppm]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "m68k.h"

#define ROMDIR "games/shinobi/roms/"
#define SW 320
#define SH 224

/* ============================ Musashi S16B harness ========================= */
/* (memory map + run loop copied from tools/shinobi_host.c -- the golden oracle) */
static uint8_t ROM[0x40000];
static uint8_t TILERAM[0x10000];   /* 0x400000 tilemap pages (16 x 0x1000)      */
static uint8_t TEXTRAM[0x1000];    /* 0x410000 text layer + scroll regs         */
static uint8_t SPRRAM[0x800];      /* 0x440000 sprite object list               */
static uint8_t PALRAM[0x1000];     /* 0x840000 palette RAM (2048 entries)        */
static uint8_t WORKRAM[0x10000];   /* 0xFF0000 work RAM                          */

static uint8_t in_service=0xff,in_p1=0xff,in_p2=0xff,dsw1=0xff,dsw2=0xff;

static unsigned io_read(unsigned addr,int size){
    unsigned wo=((addr-0xC40000)>>1)&0x1fff,val=0xffff;
    switch(wo&(0x3000/2)){
        case 0x1000/2:
            switch(wo&3){case 0:val=in_service;break;case 1:val=in_p1;break;
                         case 2:val=0xff;break;case 3:val=in_p2;break;}
            break;
        case 0x2000/2: val=(wo&1)?dsw1:dsw2; break;
        default: val=0xffff; break;
    }
    if(size==1)return val&0xff;
    return 0xff00|(val&0xff);
}
static void io_write(unsigned addr,unsigned val,int size){ (void)addr;(void)val;(void)size; }

static uint8_t *region_ptr(unsigned addr,unsigned *limit){
    addr&=0xFFFFFF;
    if(addr<0x040000){*limit=0x040000;return ROM+addr;}
    if(addr>=0x400000&&addr<0x410000){*limit=0x010000;return TILERAM+(addr-0x400000);}
    if(addr>=0x410000&&addr<0x411000){*limit=0x001000;return TEXTRAM+(addr-0x410000);}
    if(addr>=0x440000&&addr<0x440800){*limit=0x000800;return SPRRAM +(addr-0x440000);}
    if(addr>=0x840000&&addr<0x841000){*limit=0x001000;return PALRAM +(addr-0x840000);}
    if(addr>=0xFF0000){*limit=0x010000;return WORKRAM+(addr-0xFF0000);}
    return NULL;
}
static unsigned read_bytes(unsigned addr,int size){
    addr&=0xFFFFFF; uint8_t *p; unsigned limit;
    if(addr>=0xC40000&&addr<0xC44000)return io_read(addr,size);
    if(addr>=0xC60000&&addr<0xC60002)return 0xffff;
    if(addr>=0x3F0000&&addr<0x400000)return 0xffff;
    if(addr>=0xFE0000&&addr<0xFE0040)return 0xffff;
    p=region_ptr(addr,&limit);
    if(!p)return (size==1)?0xff:(size==2)?0xffff:0xffffffff;
    unsigned v=0; for(int i=0;i<size;i++)v=(v<<8)|p[i]; return v;
}
static void write_bytes(unsigned addr,unsigned val,int size){
    addr&=0xFFFFFF; uint8_t *p; unsigned limit;
    if(addr>=0xC40000&&addr<0xC44000){io_write(addr,val,size);return;}
    if(addr>=0xC60000&&addr<0xC60002)return;
    if(addr>=0x3F0000&&addr<0x400000)return;
    if(addr>=0xFE0000&&addr<0xFE0040)return;
    p=region_ptr(addr,&limit); if(!p)return;
    for(int i=size-1;i>=0;i--){p[i]=val&0xff;val>>=8;}
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
static int int_ack(int level){(void)level;m68k_set_irq(0);return M68K_INT_ACK_AUTOVECTOR;}

/* ============================== gfx decode ================================= */
/* tiles: 8x8x3bpp planar, 3 ROMs one-per-plane (bit0,bit1,bit2)              */
static uint8_t tp0[0x20000],tp1[0x20000],tp2[0x20000];
#define NTILES (0x20000/8)
static int tile_pix(int code,int x,int y){    /* pen 0..7 */
    if(code<0||code>=NTILES)return 0;
    unsigned b=(unsigned)code*8+y; int sh=7-x;
    return (((tp2[b]>>sh)&1)<<2)|(((tp1[b]>>sh)&1)<<1)|((tp0[b]>>sh)&1);
}
/* sprites: 16-bit BE region, 4bpp, 4px/word (MSN first). 4 banks of 0x10000 words */
static uint8_t sprrom[0x80000];                /* assembled BE word region        */
#define SPR_NUMBANKS (0x80000/0x20000)
static inline unsigned spr_word(unsigned wi){  /* wi = absolute word index        */
    return ((unsigned)sprrom[wi*2]<<8)|sprrom[wi*2+1];
}

/* ============================== palette =================================== */
/* paletteram word: s BBBB GGGG RRRR / x B4B3B2 G4G3G2 R4R3R2 B1B0 G1G0 R1R0   */
static void pal_rgb(int idx,int *r,int *g,int *b){
    unsigned w=((unsigned)PALRAM[idx*2]<<8)|PALRAM[idx*2+1];
    int r5=((w>>12)&0x01)|((w<<1)&0x1e);
    int g5=((w>>13)&0x01)|((w>>3)&0x1e);
    int b5=((w>>14)&0x01)|((w>>7)&0x1e);
    *r=(r5*255+15)/31; *g=(g5*255+15)/31; *b=(b5*255+15)/31;
}

static uint8_t img[SH][SW][3];
static inline uint16_t tw(int byteoff){return ((uint16_t)TILERAM[byteoff]<<8)|TILERAM[byteoff+1];}
static inline uint16_t xw(int byteoff){return ((uint16_t)TEXTRAM[byteoff]<<8)|TEXTRAM[byteoff+1];}

/* draw one tilemap layer (which: 0=foreground, 1=background). bg=opaque. */
static void draw_tilemap(int which,int opaque){
    uint16_t pages   = xw(0xe80 + which*2);
    uint16_t xscroll = xw(0xe98 + which*2);
    uint16_t yscroll = xw(0xe90 + which*2);
    for(int sy=0;sy<SH;sy++){
        for(int sx=0;sx<SW;sx++){
            int vx=(sx+xscroll)&0x3ff, vy=(sy+yscroll)&0x1ff;
            int shift=((vx>=512)?4:0)+((vy>=256)?8:0);
            int page=(pages>>shift)&0xf;
            int pcol=(vx&511)>>3, prow=(vy&255)>>3;
            int tindex=prow*64+pcol;
            uint16_t w=tw(page*0x1000 + tindex*2);
            int color=(w>>6)&0x7f, code=w&0x1fff;
            int pen=tile_pix(code, vx&7, vy&7);
            if(!opaque && pen==0) continue;       /* pen0 transparent (fg)        */
            int r,g,b; pal_rgb(color*8+pen,&r,&g,&b);   /* colorbase 0             */
            img[sy][sx][0]=r;img[sy][sx][1]=g;img[sy][sx][2]=b;
        }
    }
}

/* draw the text layer (64x28, scrolldx -192 => source x = screen x + 192). */
static void draw_text(void){
    for(int sy=0;sy<SH;sy++){
        int row=sy>>3; if(row>=28)continue;
        for(int sx=0;sx<SW;sx++){
            int tx=sx+192;
            int col=(tx>>3)&63;
            int tindex=row*64+col;
            uint16_t w=xw(tindex*2);
            int color=(w>>9)&7, code=w&0x1ff;
            int pen=tile_pix(code, tx&7, sy&7);
            if(pen==0)continue;                   /* pen0 transparent             */
            int r,g,b; pal_rgb(color*8+pen,&r,&g,&b);
            img[sy][sx][0]=r;img[sy][sx][1]=g;img[sy][sx][2]=b;
        }
    }
}

/* draw the sprite list (315-5196 / SEGA_SYS16B_SPRITES), faithful to sega16sp.cpp. */
#define SPRPAL_BASE 0x400
static void plot_spr_pix(int x,int y,int colpri){
    int pix=colpri&0xf;
    if(pix==0||pix==15)return;
    if(x<0||x>=SW||y<0||y>=SH)return;
    int palidx = SPRPAL_BASE | (colpri & 0x3ff);   /* spritepalbase | color*16|pix */
    int r,g,b; pal_rgb(palidx,&r,&g,&b);
    img[y][x][0]=r;img[y][x][1]=g;img[y][x][2]=b;
}
static void draw_sprites(void){
    const int ORIGINX=189;                          /* xpos 0xBD = screen 0        */
    uint16_t sdata[8];
    for(int e=0;e<0x800/16;e++){
        uint8_t *d=SPRRAM+e*16;
        for(int i=0;i<8;i++)sdata[i]=((uint16_t)d[i*2]<<8)|d[i*2+1];
        if(sdata[2]&0x8000)break;                   /* end of list                 */
        int bottom=sdata[0]>>8, top=sdata[0]&0xff;
        int xpos=sdata[1]&0x1ff;
        int hide=sdata[2]&0x4000;
        int flip=sdata[2]&0x100;
        int pitch=(int8_t)(sdata[2]&0xff);
        unsigned addr=sdata[3];
        int bank=(sdata[4]>>8)&0xf;
        int colpri=((sdata[4]&0xff)<<4)|(((sdata[1]>>9)&0xf)<<12);
        int vzoom=(sdata[5]>>5)&0x1f, hzoom=sdata[5]&0x1f;
        if(hide||top>=bottom)continue;
        bank%=SPR_NUMBANKS;
        unsigned sbase=0x10000u*bank;                /* word base within region     */
        int sx0=xpos-ORIGINX;
        unsigned yacc=0;
        for(int y=top;y<bottom;y++){
            addr+=pitch;
            yacc+=(unsigned)vzoom<<10;
            if(yacc&0x8000){addr+=pitch;yacc&=~0x8000u;}
            int scry=y;                              /* yoffs ~ -1, negligible      */
            if(scry<0||scry>=SH)continue;
            int xacc=4*hzoom;
            unsigned a=addr;
            if(!flip){
                int x=sx0;
                for(;;){
                    unsigned pixels=spr_word(sbase + (a&0xffff)); a++;
                    int pix;
                    pix=(pixels>>12)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr_pix(x,scry,colpri|pix);x++;}
                    pix=(pixels>> 8)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr_pix(x,scry,colpri|pix);x++;}
                    pix=(pixels>> 4)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr_pix(x,scry,colpri|pix);x++;}
                    pix=(pixels>> 0)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr_pix(x,scry,colpri|pix);x++;}
                    if(pix==15)break;
                    if(x>sx0+0x200)break;            /* safety                       */
                }
            }else{
                int x=sx0;
                for(;;){
                    unsigned pixels=spr_word(sbase + (a&0xffff)); a--;
                    int pix;
                    pix=(pixels>> 0)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr_pix(x,scry,colpri|pix);x++;}
                    pix=(pixels>> 4)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr_pix(x,scry,colpri|pix);x++;}
                    pix=(pixels>> 8)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr_pix(x,scry,colpri|pix);x++;}
                    pix=(pixels>>12)&0xf; xacc=(xacc&0x3f)+hzoom; if(xacc<0x40){plot_spr_pix(x,scry,colpri|pix);x++;}
                    if(pix==15)break;
                    if(x>sx0+0x200)break;
                }
            }
        }
    }
}

static void render_frame(void){
    memset(img,0,sizeof img);
    draw_tilemap(1,1);    /* background, opaque */
    draw_tilemap(0,0);    /* foreground, pen0 transparent */
    draw_text();          /* text, pen0 transparent */
    draw_sprites();       /* sprites on top */
}

static void write_ppm(const char *out){
    FILE *f=fopen(out,"wb"); if(!f){perror("ppm");exit(1);}
    fprintf(f,"P6\n%d %d\n255\n",SW,SH);
    fwrite(img,1,sizeof img,f); fclose(f);
}

static void load(const char *name,uint8_t *buf,unsigned len){
    char path[256]; snprintf(path,sizeof path,ROMDIR "%s",name);
    FILE *f=fopen(path,"rb"); if(!f){fprintf(stderr,"open %s\n",path);exit(1);}
    if(fread(buf,1,len,f)!=len){fprintf(stderr,"short %s\n",path);exit(1);}
    fclose(f);
}
static void load_gfx(void){
    load("shinobi4/mpr-11363.a14",tp0,0x20000);
    load("shinobi4/mpr-11364.a15",tp1,0x20000);
    load("shinobi4/mpr-11365.a16",tp2,0x20000);
    /* sprites: assemble 16-bit BE region (even/odd lanes) */
    static uint8_t s[0x20000];
    load("shinobi4/mpr-11368.b5",s,0x20000); for(int i=0;i<0x20000;i++)sprrom[0x00000+i*2  ]=s[i];
    load("shinobi4/mpr-11366.b1",s,0x20000); for(int i=0;i<0x20000;i++)sprrom[0x00000+i*2+1]=s[i];
    load("shinobi4/mpr-11369.b6",s,0x20000); for(int i=0;i<0x20000;i++)sprrom[0x40000+i*2  ]=s[i];
    load("shinobi4/mpr-11367.b2",s,0x20000); for(int i=0;i<0x20000;i++)sprrom[0x40000+i*2+1]=s[i];
}

int main(int argc,char**argv){
    int frames=(argc>1)?atoi(argv[1]):200;
    const char *out=(argc>2)?argv[2]:"build/shinobi.ppm";

    FILE *f=fopen(ROMDIR "shinobi_main.bin","rb");
    if(!f){perror("shinobi_main.bin");return 1;}
    if(fread(ROM,1,sizeof ROM,f)!=sizeof ROM){fprintf(stderr,"short main\n");return 1;}
    fclose(f);
    load_gfx();

    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_init();
    m68k_set_int_ack_callback(int_ack);
    m68k_pulse_reset();
    const int CYC=10000000/60;
    for(int fr=0;fr<frames;fr++){
        m68k_set_irq(4);
        int rem=CYC; while(rem>0)rem-=m68k_execute(rem>20000?20000:rem);
        m68k_set_irq(0);
    }

    /* stats */
    int tnz=0,xnz=0,pnz=0,snz=0;
    for(int i=0;i<0x10000;i+=2)if(TILERAM[i]||TILERAM[i+1])tnz++;
    for(int i=0;i<0x1000;i+=2)if(TEXTRAM[i]||TEXTRAM[i+1])xnz++;
    for(int i=0;i<0x1000;i+=2)if(PALRAM[i]||PALRAM[i+1])pnz++;
    for(int i=0;i<0x800;i+=2)if(SPRRAM[i]||SPRRAM[i+1])snz++;

    render_frame();
    write_ppm(out);
    printf("shinobi_shot: %d frames, PC=%06x -> %s\n",frames,m68k_get_reg(NULL,M68K_REG_PC),out);
    printf("  tilemap nz=%d text nz=%d pal nz=%d spr nz=%d  pages fg=%04x bg=%04x  scroll fg(x=%04x y=%04x) bg(x=%04x y=%04x)\n",
        tnz,xnz,pnz,snz, xw(0xe80),xw(0xe82), xw(0xe98),xw(0xe90), xw(0xe9a),xw(0xe92));
    return 0;
}
