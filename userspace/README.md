# userspace/

Native Helix user-space programs and libraries.

Programs here target the **native object API**: they receive handles to objects
and hold explicit capabilities, rather than Unix file descriptors and a root
user.

## Native API library (libhelix)

`libhelix.h` / `libhelix.c` wrap the kernel's native system-call interface
(`int $0x80`, 7-register x86-64 ABI, see `kernel/src/syscall.h`) in a clean
C API. Freestanding: no host libc, no dynamic allocation except what the
kernel heap provides.

| Area | Functions |
|------|-----------|
| Memory | `helix_alloc`, `helix_free`, `helix_map`, `helix_unmap`, `helix_brk` |
| Handles | `helix_handle_create`, `helix_handle_get`, `helix_handle_duplicate`, `helix_handle_close` |
| Objects | `helix_object_lookup`, `helix_object_retain`, `helix_object_release`, `helix_object_count` |
| Processes | `helix_exit`, `helix_yield`, `helix_thread_create` |
| Channels | `helix_channel_send`, `helix_channel_recv` |
| I/O and strings | `helix_write_fd`, `helix_read_line`, `helix_strdup`, `helix_strlen`, `helix_strcmp`, `helix_strcat` |
| Startup | `_start` (crt0: calls `main`, exits with its status) |

Build it (from `runtime/`):

```sh
make libhelix        # -> userspace/libhelix.a
```

Use it:

```c
#include "libhelix.h"

int main(int argc, char **argv)
{
    helix_write_fd(1, "hello from %s\n", "Helix");
    void *p = helix_alloc(64);
    helix_free(p);
    helix_exit(0);
}
```

```sh
gcc -ffreestanding -nostdlib -fno-pic -fno-pie -fno-stack-protector \
    -std=gnu11 -O2 -I../userspace myapp.c ../userspace/libhelix.a -o myapp
```

`helix_write_fd` supports `%s %d %u %c %p` and `ll`/`l` length prefixes only
— it is not a full printf.

## Test application

* `helix_test.c` — exercises every API group: alloc/free, object count,
  handle create/close/duplicate/get, object lookup, strings, `brk`, write,
  and yield. Prints `PASS` per group and `All tests passed.` at the end.

## Test applications (apps/)

* dhcp.c — DHCP client for optional Ethernet driver
* wifi.c — WiFi configuration tool for optional WiFi driver
* wget.c — wget client utility
* wav.c — WAV audio player for optional Sound driver
* volume.c — Volume control utility for optional Sound driver
* terminal.c — Simple terminal emulator
* browser.c — Minimal web browser with HTML/CSS/JS support
* cc.c — Simple C compiler
* x11server.c — Minimal X11 server implementation
* start-gui.c — Graphical application launcher (startx-like)
* boxwm.c — Window manager (openbox-like DE starter)

Build with:
```sh
make userspace-apps
```

The apps above target the Linux/BSD compatibility runtime (`runtime/`),
which translates Linux syscalls to the native Helix ones. New native
programs should link `libhelix.a` instead.

See [../ROADMAP.md](../ROADMAP.md) for the full plan.
