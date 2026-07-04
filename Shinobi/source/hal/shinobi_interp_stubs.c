/* Minimal libc bits for the Musashi interpreter build (no libnix; pl_support.s
 * already provides memset/memcpy/setjmp). REAL impls for what Musashi's
 * m68ki_build_opcode_table calls at init; stubs for FPU/disasm/error paths that
 * Shinobi (a 68000) never executes. */
typedef unsigned long sz_t;

unsigned long strlen(const char *s){ unsigned long n=0; while(s[n]) n++; return n; }
char *strcpy(char *d, const char *s){ char *o=d; while((*d++=*s++)); return o; }
char *strcat(char *d, const char *s){ char *o=d; while(*d) d++; while((*d++=*s++)); return o; }

/* small in-place sort (opcode table build; one-time, n modest) */
void qsort(void *base, sz_t n, sz_t sz, int (*cmp)(const void*, const void*)){
    char *a=(char*)base;
    for (sz_t i=1;i<n;i++)
        for (sz_t j=i;j>0;j--){
            char *x=a+(j-1)*sz, *y=a+j*sz;
            if (cmp(x,y)<=0) break;
            for (sz_t k=0;k<sz;k++){ char t=x[k]; x[k]=y[k]; y[k]=t; }
        }
}

/* never executed (no FPU / no error / no disasm at runtime) -- link only */
double cos(double x){ (void)x; return 0; }
double sin(double x){ (void)x; return 0; }
double cexp(double x){ (void)x; return 0; }
int sprintf(char *s, const char *f, ...){ (void)f; if(s)*s=0; return 0; }
int sscanf(const char *s, const char *f, ...){ (void)s; (void)f; return 0; }
int fprintf(void *p, const char *f, ...){ (void)p; (void)f; return 0; }
unsigned long fwrite(const void *p, unsigned long a, unsigned long b, void *f){ (void)p;(void)f; return a*b; }
int vfprintf(void *p, const char *f, void *a){ (void)p;(void)f;(void)a; return 0; }
void exit(int c){ (void)c; for(;;); }
void abort(void){ for(;;); }
char __sF[256];

/* libgcc helpers: only reached via the FPU/float path, which a 68000 never
 * executes -> stub to break the libgcc dependency cascade. */
float  __mulsf3(float a, float b){ (void)a;(void)b; return 0; }
float  __addsf3(float a, float b){ (void)a;(void)b; return 0; }
float  __subsf3(float a, float b){ (void)a;(void)b; return 0; }
float  __divsf3(float a, float b){ (void)a;(void)b; return 0; }
double __muldf3(double a, double b){ (void)a;(void)b; return 0; }
double __divdf3(double a, double b){ (void)a;(void)b; return 0; }
double __extendsfdf2(float a){ (void)a; return 0; }
float  __truncdfsf2(double a){ (void)a; return 0; }
double __floatsidf(int a){ (void)a; return 0; }
/* 64-bit integer division helpers libgcc would normally provide. Musashi's
 * softfloat paths reference them even though Shinobi runs as a plain 68000.
 * Keep real implementations here so the no-libc/no-libgcc hunk link stays
 * self-contained. */
typedef unsigned long long u64;
typedef long long s64;
static u64 udivmod64(u64 n, u64 d, u64 *rem)
{
    u64 q = 0, r = 0;
    if (d == 0) { if (rem) *rem = 0; return 0; }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (u64)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}
u64 __udivdi3(u64 a, u64 b) { return udivmod64(a, b, 0); }
u64 __umoddi3(u64 a, u64 b) { u64 r; udivmod64(a, b, &r); return r; }
s64 __divdi3(s64 a, s64 b)
{
    int neg = (a < 0) ^ (b < 0);
    u64 q = udivmod64(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, 0);
    return neg ? -(s64)q : (s64)q;
}
s64 __moddi3(s64 a, s64 b)
{
    u64 r; udivmod64(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, &r);
    return a < 0 ? -(s64)r : (s64)r;
}
