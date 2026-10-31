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
    /* ---- Linux syscall mapping: all known-call coverage ----
     * Native values are pinned to literals equal to the enum in
     * kernel/src/syscall.h — runtime cannot include kernel headers,
     * so this is the cross-tree drift check. */
    check(linux_translate_number(0) == 20,  "read -> native 20");
    check(linux_translate_number(1) == 21,  "write -> native 21");
    check(linux_translate_number(2) == 11,  "open -> native 11");
    check(linux_translate_number(3) == 12,  "close -> native 12");
    check(linux_translate_number(9) == 4,   "mmap -> native 4");
    check(linux_translate_number(11) == 5,  "munmap -> native 5");
    check(linux_translate_number(12) == 24, "brk -> native 24");
    check(linux_translate_number(24) == 8,  "sched_yield -> native 8");
    check(linux_translate_number(32) == 13, "dup -> native 13");
    check(linux_translate_number(33) == 13, "dup2 -> native 13");
    check(linux_translate_number(56) == 6,  "clone -> native 6");
    check(linux_translate_number(59) == 25, "execve -> native 25");
    check(linux_translate_number(60) == 23, "exit -> native 23");
    check(linux_translate_number(85) == 11, "creat -> native 11");
    check(linux_translate_number(231) == 23, "exit_group -> native 23");
    check(linux_translate_number(292) == 13, "dup3 -> native 13");

    /* Known Linux calls with no native equivalent -> ENOSYS (-1). */
    check(linux_translate_number(16) == -1,  "ioctl ENOSYS");
    check(linux_translate_number(39) == -1,  "getpid ENOSYS");
    check(linux_translate_number(41) == -1,  "socket ENOSYS");
    check(linux_translate_number(83) == -1,  "mkdir ENOSYS");
    check(linux_translate_number(217) == -1, "getdents64 ENOSYS");
    check(linux_translate_number(257) == -1, "openat ENOSYS");
    check(linux_translate_number(318) == -1, "getrandom ENOSYS");

    /* Out-of-table numbers -> ENOSYS, never a bogus native index. */
    check(linux_translate_number(-1) == -1,    "negative -> -1");
    check(linux_translate_number(512) == -1,   "past table -> -1");
    check(linux_translate_number(9999) == -1,  "absurd -> -1");

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

    /* ---- Translate path (host-safe: no int $0x80 can fire) ----
     * Unmapped Linux calls return Linux-ENOSYS before any native call;
     * an unmapped fd returns EBADF before any native call. */
    check(linux_translate_syscall(16, 0, 0, 0, 0, 0, 0, 0) == (uint64_t)-38,
          "translate ioctl -> ENOSYS -38");
    check(linux_translate_syscall(0, 7, 0, 0, 0, 0, 0, 0) == (uint64_t)-9,
          "translate read bad fd -> EBADF -9");

    if (g_failures == 0) {
        say("PASS: all checks");
        return 0;
    }
    say("FAILURES detected");
    return 1;
}