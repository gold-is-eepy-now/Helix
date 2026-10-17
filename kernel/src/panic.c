/* panic.c — a small printf and a panic path.
 *
 * Before this existed, a failure meant a silent hang. Now we can fail loudly:
 * print what went wrong and where, then stop. That is the difference between
 * "it doesn't work" and "it says exactly why".
 */
#include "panic.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "io.h"

/* Format `value` into `buf` and return the length (no NUL terminator). */
static int format_unsigned(char *buf, uint64_t value, unsigned base, bool upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[70];
    int n = 0;

    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value != 0 && n < (int)sizeof(tmp)) {
            tmp[n++] = digits[value % base];
            value /= base;
        }
    }
    for (int i = 0; i < n; i++) {
        buf[i] = tmp[n - 1 - i];
    }
    return n;
}

static void emit_number(uint64_t value, unsigned base, bool upper, bool negative,
                        int width, char pad) {
    char buf[70];
    int len = format_unsigned(buf, value, base, upper);
    int total = len + (negative ? 1 : 0);

    for (int i = total; i < width; i++) {
        console_putc(pad);
    }
    if (negative) {
        console_putc('-');
    }
    for (int i = 0; i < len; i++) {
        console_putc(buf[i]);
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            console_putc(*p);
            continue;
        }

        p++;
        if (*p == '\0') {
            break;
        }

        bool zero_pad = false;
        if (*p == '0') {
            zero_pad = true;
            p++;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        bool is_long = false;
        while (*p == 'l') {
            is_long = true;
            p++;
        }

        char pad = zero_pad ? '0' : ' ';
        switch (*p) {
            case 's': {
                const char *s = va_arg(args, const char *);
                if (s == NULL) {
                    s = "(null)";
                }
                console_write(s);
                break;
            }
            case 'c':
                console_putc((char)va_arg(args, int));
                break;
            case 'd': {
                int64_t v = is_long ? va_arg(args, long long) : va_arg(args, int);
                if (v < 0) {
                    emit_number((uint64_t)(-v), 10, false, true, width, pad);
                } else {
                    emit_number((uint64_t)v, 10, false, false, width, pad);
                }
                break;
            }
            case 'u': {
                uint64_t v = is_long ? va_arg(args, unsigned long long)
                                     : va_arg(args, unsigned int);
                emit_number(v, 10, false, false, width, pad);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t v = is_long ? va_arg(args, unsigned long long)
                                     : va_arg(args, unsigned int);
                emit_number(v, 16, *p == 'X', false, width, pad);
                break;
            }
            case 'p':
                console_write("0x");
                emit_number(va_arg(args, uintptr_t), 16, false, false, 16, '0');
                break;
            case '%':
                console_putc('%');
                break;
            default:
                console_putc('%');
                console_putc(*p);
                break;
        }
    }

    va_end(args);
}

__attribute__((noreturn))
void halt_forever(void) {
    cpu_disable_interrupts();
    for (;;) {
        cpu_halt();
    }
}

__attribute__((noreturn))
void panic(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    console_set_fg(CONSOLE_BRIGHT_RED);
    console_write("\n*** KERNEL PANIC ***\n");
    console_reset_color();

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            console_putc(*p);
            continue;
        }
        p++;
        switch (*p) {
            case 's': {
                const char *s = va_arg(args, const char *);
                console_write(s ? s : "(null)");
                break;
            }
            case 'd':
                console_write_dec((uint64_t)va_arg(args, int));
                break;
            case 'u':
                console_write_dec(va_arg(args, unsigned int));
                break;
            case 'x':
                console_write_hex(va_arg(args, unsigned int));
                break;
            case 'p':
                console_write_hex(va_arg(args, uintptr_t));
                break;
            case '%':
                console_putc('%');
                break;
            default:
                console_putc('%');
                console_putc(*p);
                break;
        }
    }
    console_putc('\n');
    console_write("System halted.\n");
    va_end(args);

    halt_forever();
}
