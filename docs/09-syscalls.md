# M9: Native system-call ABI

Native `.hlx` applications use x86-64 `syscall`/`sysret`. The DPL-3
`int 0x80` path remains for the controlled entry/exit self-test.

The call number is in `RAX`; arguments use `RDI`, `RSI`, and `RDX`; the result
returns in `RAX`. A result of `-1` means the kernel rejected the request.

| Number | Call | Behavior |
|---:|---|---|
| 0 | `exit(status)` | Ends the current app and returns to its launcher. |
| 1 | `write(buffer, length)` | Writes at most 256 bytes from a mapped user buffer to the console. |
| 2 | `read(buffer, capacity)` | Reads at most 128 bytes into a writable user buffer, echoing input until newline or capacity. Backspace edits the current line. |
| 3 | `clear()` | Clears the framebuffer and serial terminal and homes the cursor. |
| 4 | `sysinfo()` | Prints the existing kernel system-information panel. |
| 5 | `edit()` | Opens Helix Edit; Ctrl-S creates a new `.TXT` on FAT32 USB0. |
| 6 | `drivers()` | Lists registered device drivers. |
| 7 | `uart_write_byte(capability, byte)` | Sends one non-newline byte through a process-held UART control capability. |
| 8 | `uart_capability()` | Returns the UART capability granted to the boot shell, or `-1` if unavailable. |
| 9 | `usb_info()` | Prints discovered USB host-controller and attached mass-storage information. |
| 10 | `usb_service_channel()` | Returns the boot shell's bounded USB service request handle, or `-1` if unavailable. |
| 11 | `usb_read_sector(channel, lba, buffer)` | Sends a sector read through the USB storage worker and copies one sector into a validated writable user buffer. |
| 12 | `usb_write_sector(channel, lba, buffer)` | Sends one sector from a validated readable user buffer through the USB storage worker. Used by the current FAT32 backend. |
| 20 | `version()` | Prints the version and name compiled into the running kernel. |

The implementation masks IF, TF, and DF on syscall entry. Each synchronous
app launch now gets a guarded NX kernel transition stack, reclaimed when the
app exits or faults. Maskable interrupts stay disabled while an app is
running, so `read` polls input and user processes cannot be preempted or run in
parallel yet. User pointer ranges are checked against the active address space
before kernel access. Per-launch stack allocation and reclamation passed in
QEMU. A separate ring-3 denial check confirms that a process with no USB grant
cannot obtain a capability or read with a forged handle.

The shell app supports `help`, `echo`, `version`, `sysinfo`, `neofetch`,
`about`, `calc A OP B`, `edit`, `drivers`, `usb`, `usb read LBA`, `usb ls usb0`,
`usb cat NAME.TXT`, `uart X` (send one byte through the
UART driver using its process-held capability), `clear`, and `exit`.
`neofetch` and `about` are aliases for `sysinfo`. `calc` evaluates signed
32-bit integer addition, subtraction, multiplication, and division. Helix Edit
is launched from the user shell; the rescue shell no longer exposes it. The
user shell's `ls`, `cd`, and `pwd` navigate the attached FAT32 volume using
short 8.3 directory names. `cd ..` moves to the parent and `cd /` returns to
the volume root. Long filenames and HFS directory navigation are not supported
yet.
The editor and system-information panel are currently invoked through narrow kernel
services. Helix Edit can create a new root-level `.TXT` file on a FAT32 USB volume
with Ctrl-S; it accepts short 8.3 names, writes at most 8 KiB, and refuses to
overwrite an existing file. Loading files, overwriting, and saving into
subdirectories are not implemented. The shell app is
loaded as a Limine boot module and started as the initial user process. The
interactive shell and startup/fallback path passed in QEMU. The fast syscall
entry and the `sysinfo`, `neofetch`, `edit`, and `drivers` commands passed in
QEMU. `uart z` and USB sector access passed through the process-held service
channel. Recursive FAT32 listing and an editor save were verified against a
disposable QEMU disk, including host-side readback of the saved text.

M9 supports one synchronous user process at a time. Concurrent user scheduling
is later work; filesystem-backed app discovery depends on HFS, planned for
M11. This bootstrap ABI does not provide Linux/BSD compatibility, file handles,
or user accounts.
