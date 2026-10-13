#ifndef WEB_STDIO_H
#define WEB_STDIO_H
typedef __SIZE_TYPE__ size_t;
typedef struct { int unused; } FILE;
FILE *fopen(const char *,const char *);
int fclose(FILE *);
int fseek(FILE *,long,int);
long ftell(FILE *);
size_t fread(void *,size_t,size_t,FILE *);
size_t fwrite(const void *,size_t,size_t,FILE *);
void rewind(FILE *);
int fflush(FILE *);
int sprintf(char *,const char *,...);
int snprintf(char *,size_t,const char *,...);
int remove(const char *);
int rename(const char *,const char *);
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
