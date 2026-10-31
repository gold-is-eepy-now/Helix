/* runtime_test.c — Runtime syscall translation self-check.
 *
 * Verifies that Linux and BSD syscall number translation produce the
 * expected native Helix syscall indices. No kernel required; this is a
 * freestanding userspace test binary.
 *
 * Build:  make -C runtime test
 * Run:    ./runtime_test
 *
 * Exits 0 on pass, 1 on any failed check.
 */

#include <stdint.h>
#include "linux_runtime.h"
#include "linux_syscall.h"
#include "bsd_syscall.h"

static int g_failures = 0;

int main(void);

/* Raw Linux write(1, buf, len) via syscall — freestanding printf replacement. */
static void raw_write(const char *s, long len) {
    register long rax __asm__("rax") = 1;   /* sys_write */
    register long rdi __asm__("rdi") = 1;   /* stdout */
    register long rsi __asm__("rsi") = (long)s;
    register long rdx __asm__("rdx") = len;
    __asm__ volatile ("syscall"
        : "+r"(rax)
        : "r"(rdi), "r"(rsi), "r"(rdx)
        : "rcx", "r11", "memory");
}

/* Print a message with trailing newline. */
static void say(const char *s) {
    long n = 0;
    while (s[n] != '\0') n++;
    raw_write(s, n);
    raw_write("\n", 1);
}

/* Freestanding entry point: call main, then raw-exit via Linux syscall 60.
 * Without libc there is no CRT startup, so _start must do this itself. */
__attribute__((noreturn))
void _start(void) {
    int rc = main();
    /* Linux exit(60): rdi = status, rax = 60, syscall. */
    __asm__ volatile (
        "movq %0, %%rdi\n"
        "movq $60, %%rax\n"
        "syscall\n"
        :
        : "r"((long)rc)
        : "rax", "rdi"
    );
    __builtin_unreachable();
}

static void check(int cond, const char *name) {
    if (cond) {
        say(name);
    } else {
        raw_write("FAIL: ", 6);
        say(name);
        g_failures++;
    }
}

int main(void) {
    /* ---- Linux syscall mapping ---- */
    check(linux_translate_number(0) == SYSCALL_READ,  "linux read");
    check(linux_translate_number(1) == SYSCALL_WRITE, "linux write");
    check(linux_translate_number(12) == SYSCALL_BRK,  "linux brk");
    check(linux_translate_number(60) == SYSCALL_EXIT, "linux exit");
    check(linux_translate_number(2) == SYSCALL_HANDLE_CREATE,  "linux open");
    check(linux_translate_number(3) == SYSCALL_HANDLE_CLOSE,   "linux close");
    check(linux_translate_number(9999) == -1, "linux unmapped -> -1");

    /* ---- BSD syscall mapping (pure, no invocation) ---- */
    check(bsd_map_to_native(3) == SYSCALL_READ,  "bsd read");
    check(bsd_map_to_native(4) == SYSCALL_WRITE, "bsd write");
    check(bsd_map_to_native(1) == SYSCALL_EXIT,  "bsd exit");
    check(bsd_map_to_native(5) == SYSCALL_HANDLE_CREATE, "bsd open");
    check(bsd_map_to_native(6) == SYSCALL_HANDLE_CLOSE,  "bsd close");
    check(bsd_map_to_native(189) == SYSCALL_BRK, "bsd brk");
    check(bsd_map_to_native(9999) == -1, "bsd unmapped -> -1");

    /* ---- FD mapping round-trip ---- */
    linux_fd_table_init();
    linux_set_fd_handle(0, 42);
    check(linux_fd_to_handle(0) == 42, "fd round-trip");
    check(bsd_fd_to_handle(0) == 42,   "bsd fd shares table");
    check(linux_fd_to_handle(-1) == 0, "bad fd -> 0");
    check(linux_fd_to_handle(9999) == 0, "out-of-range fd -> 0");

    if (g_failures == 0) {
        say("PASS: all checks");
        return 0;
    }
    say("FAILURES detected");
    return 1;
}