/* stdio.h — formatted and file I/O for the Helix C runtime.
 *
 * Output is unbuffered: each call makes a write() system call. That keeps the
 * runtime small and makes console output appear in order. */
#ifndef HELIX_LIBC_STDIO_H
#define HELIX_LIBC_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct _helix_file FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int sprintf(char *dst, const char *fmt, ...);
int snprintf(char *dst, size_t cap, const char *fmt, ...);
int vsnprintf(char *dst, size_t cap, const char *fmt, va_list ap);

int puts(const char *s);
int fputs(const char *s, FILE *f);
int putchar(int c);
int fputc(int c, FILE *f);
int getchar(void);
int fgetc(FILE *f);
char *fgets(char *buf, int n, FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
size_t fread(void *p, size_t size, size_t n, FILE *f);

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
int fflush(FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);

#endif
