#include "emu_core.h"
#include <stdio.h>
#include <stdlib.h>

/* The application profile uses in-memory resources. No host OS files exist. */
FILE *fopen(const char *a,const char *b){(void)a;(void)b;return 0;}
int fclose(FILE *f){(void)f;return 0;}
int fseek(FILE *f,long o,int w){(void)f;(void)o;(void)w;return -1;}
long ftell(FILE *f){(void)f;return -1;}
size_t fread(void *d,size_t s,size_t n,FILE *f){(void)d;(void)s;(void)n;(void)f;return 0;}
size_t fwrite(const void *d,size_t s,size_t n,FILE *f){(void)d;(void)s;(void)n;(void)f;return 0;}
void rewind(FILE *f){(void)f;}
int fflush(FILE *f){(void)f;return 0;}
int sprintf(char *d,const char *fmt,...){(void)fmt;d[0]=0;return 0;}
int snprintf(char *d,size_t n,const char *fmt,...){(void)fmt;if(n)d[0]=0;return 0;}
int remove(const char *p){(void)p;return -1;}
int rename(const char *a,const char *b){(void)a;(void)b;return -1;}
void *malloc(size_t s){(void)s;return 0;}
void free(void *p){(void)p;}
void *memcpy(void *d,const void *s,size_t n){u8 *q=d;const u8 *p=s;for(size_t i=0;i<n;i++)q[i]=p[i];return d;}
void *memset(void *d,int v,size_t n){u8 *q=d;for(size_t i=0;i<n;i++)q[i]=(u8)v;return d;}
static u8 scratch[8192];
u8 *web_scratch(void){return scratch;}
u32 web_timer_id(int i){u32 id=0;emu_timer_info(i,&id,0);return id;}
u32 web_timer_interval(int i){u32 ms=0;emu_timer_info(i,0,&ms);return ms;}
