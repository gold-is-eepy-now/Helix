/* stdio.c — formatted output and file I/O (see include/stdio.h).
 *
 * One formatting core (vfmt) serves printf, fprintf, sprintf and snprintf. It
 * writes through a sink: to a caller's buffer for the *sprintf family, or to a
 * FILE in small pieces, so long output does not need one big buffer. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct _helix_file {
    int fd;
    int is_std;
};

static struct _helix_file g_stdin = { 0, 1 };
static struct _helix_file g_stdout = { 1, 1 };
static struct _helix_file g_stderr = { 2, 1 };

FILE *stdin = &g_stdin;
FILE *stdout = &g_stdout;
FILE *stderr = &g_stderr;

/* -- Formatting core -------------------------------------------------------- */

typedef struct {
    FILE *file;          /* set when writing to a FILE */
    char *dst;           /* set when writing into a caller's buffer */
    size_t cap;
    size_t total;        /* characters produced, as printf returns */
    char pending[128];
    size_t np;
} sink_t;

static void sink_flush(sink_t *s) {
    if (s->file != NULL && s->np > 0) {
        fwrite(s->pending, 1, s->np, s->file);
        s->np = 0;
    }
}

static void sink_put(sink_t *s, char c) {
    s->total++;
    if (s->file != NULL) {
        s->pending[s->np++] = c;
        if (s->np == sizeof(s->pending)) sink_flush(s);
    } else if (s->dst != NULL && s->total - 1 < s->cap) {
        /* Keep room for the terminating NUL. */
        if (s->total < s->cap) s->dst[s->total - 1] = c;
    }
}

static void sink_pad(sink_t *s, char c, int n) {
    while (n-- > 0) sink_put(s, c);
}

static void sink_str(sink_t *s, const char *str, int width, int left) {
    size_t len = strlen(str);
    int pad = width > (int)len ? width - (int)len : 0;
    if (!left) sink_pad(s, ' ', pad);
    for (size_t i = 0; i < len; i++) sink_put(s, str[i]);
    if (left) sink_pad(s, ' ', pad);
}

/* Unsigned number in `base`, with sign, width and zero padding. */
static void sink_num(sink_t *s, unsigned long long v, int base, int upper,
                     int neg, int width, int zero, int left, int prefix_hex) {
    static const char lo[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *digits = upper ? up : lo;
    char tmp[32];
    int n = 0;
    do {
        tmp[n++] = digits[v % (unsigned)base];
        v /= (unsigned)base;
    } while (v != 0 && n < (int)sizeof(tmp));

    int len = n + (neg ? 1 : 0) + (prefix_hex ? 2 : 0);
    int pad = width > len ? width - len : 0;
    if (!left && !zero) sink_pad(s, ' ', pad);
    if (neg) sink_put(s, '-');
    if (prefix_hex) {
        sink_put(s, '0');
        sink_put(s, upper ? 'X' : 'x');
    }
    if (!left && zero) sink_pad(s, '0', pad);
    while (n > 0) sink_put(s, tmp[--n]);
    if (left) sink_pad(s, ' ', pad);
}

/* Supports %% %c %s %d %i %u %x %X %p, the width and '-'/'0' flags, and the
 * length modifiers l, ll, h and z (on 64-bit, l and ll are the same size). */
static void vfmt(sink_t *s, const char *fmt, va_list ap) {
    for (; *fmt != '\0'; fmt++) {
        if (*fmt != '%') {
            sink_put(s, *fmt);
            continue;
        }
        fmt++;
        int left = 0, zero = 0, width = 0, longs = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else break;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') {
            if (*fmt != 'h') longs++;
            fmt++;
        }
        if (*fmt == '\0') break;
        switch (*fmt) {
        case '%':
            sink_put(s, '%');
            break;
        case 'c':
            sink_put(s, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            sink_str(s, str != NULL ? str : "(null)", width, left);
            break;
        }
        case 'd':
        case 'i': {
            long long v = longs ? va_arg(ap, long long) : (long long)va_arg(ap, int);
            int neg = v < 0;
            unsigned long long mag = neg ? 0ULL - (unsigned long long)v : (unsigned long long)v;
            sink_num(s, mag, 10, 0, neg, width, zero, left, 0);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long long v = longs ? va_arg(ap, unsigned long long)
                                         : (unsigned long long)va_arg(ap, unsigned int);
            int base = *fmt == 'u' ? 10 : 16;
            sink_num(s, v, base, *fmt == 'X', 0, width, zero, left, 0);
            break;
        }
        case 'p':
            sink_num(s, (unsigned long long)(uintptr_t)va_arg(ap, void *), 16, 0, 0,
                     width, zero, left, 1);
            break;
        default:
            /* Unknown conversion: print it as written. */
            sink_put(s, '%');
            sink_put(s, *fmt);
            break;
        }
    }
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    sink_t s;
    memset(&s, 0, sizeof(s));
    s.file = f;
    vfmt(&s, fmt, ap);
    sink_flush(&s);
    return (int)s.total;
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vsnprintf(char *dst, size_t cap, const char *fmt, va_list ap) {
    sink_t s;
    memset(&s, 0, sizeof(s));
    s.dst = dst;
    s.cap = cap;
    vfmt(&s, fmt, ap);
    if (cap > 0) dst[s.total < cap ? s.total : cap - 1] = '\0';
    return (int)s.total;
}

int snprintf(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *dst, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, (size_t)-1, fmt, ap);
    va_end(ap);
    return n;
}

/* -- Characters and lines --------------------------------------------------- */

size_t fwrite(const void *p, size_t size, size_t n, FILE *f) {
    size_t total = size * n;
    const char *c = p;
    size_t done = 0;
    while (done < total) {
        ssize_t w = write(f->fd, c + done, total - done);
        if (w <= 0) break;
        done += (size_t)w;
    }
    return size != 0 ? done / size : 0;
}

size_t fread(void *p, size_t size, size_t n, FILE *f) {
    size_t total = size * n;
    char *c = p;
    size_t done = 0;
    while (done < total) {
        ssize_t r = read(f->fd, c + done, total - done);
        if (r <= 0) break;
        done += (size_t)r;
    }
    return size != 0 ? done / size : 0;
}

int fputc(int c, FILE *f) {
    char ch = (char)c;
    return fwrite(&ch, 1, 1, f) == 1 ? (unsigned char)c : EOF;
}

int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

int putchar(int c) {
    return fputc(c, stdout);
}

int fgetc(FILE *f) {
    unsigned char c;
    return fread(&c, 1, 1, f) == 1 ? c : EOF;
}

int getchar(void) {
    return fgetc(stdin);
}

char *fgets(char *buf, int n, FILE *f) {
    if (n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    buf[i] = '\0';
    return buf;
}

/* -- Files ------------------------------------------------------------------ */

FILE *fopen(const char *path, const char *mode) {
    int flags;
    if (mode[0] == 'r') flags = mode[1] == '+' ? O_RDWR : O_RDONLY;
    else if (mode[0] == 'w') flags = (mode[1] == '+' ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
    else if (mode[0] == 'a') flags = (mode[1] == '+' ? O_RDWR : O_WRONLY) | O_CREAT;
    else {
        errno = EINVAL;
        return NULL;
    }
    int fd = open(path, flags);
    if (fd < 0) return NULL;
    if (mode[0] == 'a') lseek(fd, 0, SEEK_END);

    FILE *f = malloc(sizeof(*f));
    if (f == NULL) {
        close(fd);
        return NULL;
    }
    f->fd = fd;
    f->is_std = 0;
    return f;
}

int fclose(FILE *f) {
    int r = close(f->fd);
    if (!f->is_std) free(f);
    return r;
}

/* Output is unbuffered, so there is nothing to flush. */
int fflush(FILE *f) {
    (void)f;
    return 0;
}
