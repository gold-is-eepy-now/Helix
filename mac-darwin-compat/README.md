# Mac/Darwin Compatibility Subsystem (mac-darwin-compat)

This subsystem provides macOS / Darwin application binary translation and compatibility for Helix OS.

## Architecture

The Darwin translation layer sits above Helix's microkernel and native object/capability architecture. It acts as an execution environment and translation bridge for both advanced macOS CLI utilities and macOS GUI applications (running under Helix's X11 server).

```
+-------------------------------------------------------+
|  Advanced Mac CLI Tools   |   Advanced Mac GUI Apps   |
| (sw_vers, defaults, etc.) |  (Calculator, TextEdit)   |
+---------------------------+---------------------------+
|              AppKit / Cocoa to X11 Bridge             |
|   (Traffic lights, window styling, event dispatch)    |
+-------------------------------------------------------+
|        Mac-like Virtual Filesystem (HFS+/APFS)        |
|  - Case-insensitive, case-preserving path lookup     |
|  - macOS tree (/System, /Library, /Applications, ...)  |
|  - Extended attributes (xattr), quarantine, metadata  |
|  - Property List (plist) engine & Bundle loader       |
+-------------------------------------------------------+
|           Mach & Darwin Syscall Translator            |
|  - BSD syscall class (0x2000000 | sys_nr)             |
|  - Mach trap class (0x1000000 | trap_nr) & traps      |
|  - Darwin sysctl (kern.*, hw.*) & Mach IPC ports      |
+-------------------------------------------------------+
|               Helix Native Syscall ABI                |
|             (int $0x80 / native kernel)               |
+-------------------------------------------------------+
```

## Features

### 1. Mach-O 64-bit Loader & Parser (`mach_o.c`)
- Supports 64-bit Mach-O headers (`MH_MAGIC_64`, 0xFEEDFACF).
- Parses `LC_SEGMENT_64` (__TEXT, __DATA, __LINKEDIT) and section headers (__text, __data, __bss).
- Supports entrypoint extraction from both `LC_MAIN` (macOS 10.8+) and `LC_UNIXTHREAD` (classic Darwin thread states).
- Parses symbol tables (`LC_SYMTAB`, `nlist_64`), dynamic symbols (`LC_DYSYMTAB`), and dynamic libraries (`LC_LOAD_DYLIB`).

### 2. Darwin & Mach Syscall and Trap Translation (`darwin_syscall.c`)
- Fully translates Darwin BSD syscalls into native Helix system calls:
  - `read` (Darwin 3) -> Helix `SYSCALL_READ` (20)
  - `write` (Darwin 4) -> Helix `SYSCALL_WRITE` (21)
  - `open` (Darwin 5) -> Helix `SYSCALL_HANDLE_CREATE` (11)
  - `close` (Darwin 6) -> Helix `SYSCALL_HANDLE_CLOSE` (12)
  - `mmap` (Darwin 197) -> Helix `SYSCALL_MAP` (4)
  - `munmap` (Darwin 73) -> Helix `SYSCALL_UNMAP` (5)
  - `exit` (Darwin 1) -> Helix `SYSCALL_EXIT` (23)
  - `sched_yield` (Darwin 327) -> Helix `SYSCALL_THREAD_YIELD` (8)
  - `execve` (Darwin 59) -> Helix `SYSCALL_EXECVE` (25)
  - `fork` / `bsdthread_create` -> Helix `SYSCALL_THREAD_CREATE` (6)
- Mach trap dispatcher:
  - Negative trap IDs (-26 = mach_reply_port, -27 = thread_self, -28 = task_self, -29 = host_self, -31 = mach_msg).
  - Traps with `DARWIN_CLASS_MACH` prefix (0x1000000).
- Darwin `sysctl` and `sysctlbyname`:
  - `kern.ostype` ("Darwin"), `kern.osrelease` ("23.5.0"), `kern.version`, `kern.hostname`.
  - `hw.machine` ("x86_64"), `hw.model` ("Macmini8,1"), `hw.ncpu` (4), `hw.pagesize` (4096), `hw.memsize`.

### 3. Mach Ports & IPC (`mach_port.c`)
- Port allocation, deallocation, and rights management (`mach_port_allocate`, `mach_port_deallocate`, `mach_port_insert_right`).
- Special ports: `mach_task_self()` and `mach_host_self()`.
- Synchronous and asynchronous Mach IPC messaging (`mach_msg_trap_impl`).

### 4. Mac-like Filesystem (`mac_fs.c`)
- Standard macOS directory tree:
  - `/System/Library/Frameworks` (Foundation, AppKit, CoreGraphics, CoreFoundation, IOKit)
  - `/System/Library/CoreServices` (`SystemVersion.plist`)
  - `/Library/Application Support` & `/Library/Preferences`
  - `/Applications` (holds macOS `.app` bundles)
  - `/Users/helix/` (Desktop, Documents, Downloads, Library)
  - `/Volumes/Macintosh HD`
  - `/private/var`, `/private/etc`, `/private/tmp`
  - Darwin symlinks: `/etc` -> `/private/etc`, `/var` -> `/private/var`, `/tmp` -> `/private/tmp`
- Case-insensitivity & case-preservation:
  - Accessing `/applications/calculator.app` resolves to `/Applications/Calculator.app`.
- Extended attributes (`xattr`):
  - `mac_fs_setxattr`, `mac_fs_getxattr`, `mac_fs_listxattr`, `mac_fs_removexattr`.
  - Handles `com.apple.quarantine`, `com.apple.FinderInfo`, and `com.apple.ResourceFork`.

### 5. Property List Engine (`plist.c`)
- XML plist parser and serializer.
- Strongly-typed DOM tree: Dictionaries, Arrays, Strings, Integers, and Booleans.

### 6. Application Bundle Loader (`bundle.c`)
- Recognizes `.app` bundles (`App.app/Contents/Info.plist`, `Contents/MacOS/App`).
- Parses bundle identifier (`CFBundleIdentifier`), executable name (`CFBundleExecutable`), package type (`CFBundlePackageType`), and version.

### 7. Advanced Mac CLI Utilities (`src/cli/`)
- `sw_vers`: Prints macOS version, build number, and product details.
- `defaults`: Reads and writes user preferences in `~/Library/Preferences/<domain>.plist`.
- `plutil`: Lints and prints `.plist` files.
- `uname`: Darwin-compatible system information.
- `otool`: Inspects Mach-O load commands, segment headers, and libraries.
- `open`: Launches files and `.app` bundles.

### 8. Mac GUI & AppKit-to-X11 Translation Layer (`mac_appkit_x11.c`)
- Interfaces directly with Helix's X11 server (`userspace/apps/x11server.c`).
- Authentic macOS window frame:
  - Left-hand traffic light buttons: Close (Red `#FF5F56`), Minimize (Yellow `#FFBD2E`), Zoom (Green `#27C93F`).
  - Sleek titlebar, centered title text, and clean window chrome.
  - Generates standard X11 protocol wire requests (`X_CreateWindow`, `X_MapWindow`, `X_PolyFillRectangle`).
- Built-in Mac GUI Apps:
  - `Calculator.app`: Interactive 4-function Mac Calculator with numeric LCD display and orange operator keypad.
  - `TextEdit.app`: Document editor with toolbar, editing buffer, and live status bar.

## Build and Test

```sh
# Run the Mac/Darwin compatibility test suite
make -C mac-darwin-compat test

# Build the Darwin translation binary
make -C mac-darwin-compat all

# Run top-level Helix tests (includes mac-darwin-compat)
make test
```
