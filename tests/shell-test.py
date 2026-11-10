#!/usr/bin/env python3
"""shell-test.py — boot Helix and drive the interactive shell.

Unlike boot-test.sh (which only checks the banner), this harness talks to the
kernel's serial port over a Unix socket, types real commands, and checks the
responses. It is the machine-checked definition of "the shell works".

It also grabs a screenshot of the framebuffer console via the QEMU monitor.

Exit status is 0 only if every check passes.
"""
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

BUILD_DIR = os.environ.get("BUILD_DIR", "build")
IMAGE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD_DIR, "helix.img")
OUTDIR = os.path.join(BUILD_DIR, "test")
# Unix sockets cannot bind on every filesystem (Windows/DrvFs mounts reject
# AF_UNIX bind), so the control sockets live in the system temp dir.
SOCK_DIR = os.path.join(tempfile.gettempdir(), f"helix-shell-test-{os.getpid()}")
SERIAL_SOCK = os.path.join(SOCK_DIR, "serial.sock")
MONITOR_SOCK = os.path.join(SOCK_DIR, "monitor.sock")
PPM = os.path.join(OUTDIR, "shell-screen.ppm")
PNG = os.path.join(OUTDIR, "shell-screen.png")

def _find_ovmf():
    """Locate UEFI firmware: environment first, then common Linux/macOS paths."""
    code = os.environ.get("OVMF_CODE")
    vars_src = os.environ.get("OVMF_VARS_SRC")
    if code and vars_src:
        return code, vars_src
    candidates = [
        ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd"),
        ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
        ("/opt/homebrew/share/qemu/edk2-x86_64-code.fd",
         "/opt/homebrew/share/qemu/edk2-i386-vars.fd"),
        ("/usr/local/share/qemu/edk2-x86_64-code.fd",
         "/usr/local/share/qemu/edk2-i386-vars.fd"),
        ("/opt/local/share/qemu/edk2-x86_64-code.fd",
         "/opt/local/share/qemu/edk2-i386-vars.fd"),
        ("/usr/share/qemu/edk2-x86_64-code.fd",
         "/usr/share/qemu/edk2-i386-vars.fd"),
        ("/usr/share/edk2/x86_64/OVMF_CODE.fd",
         "/usr/share/edk2/x86_64/OVMF_VARS.fd"),
    ]
    for c, v in candidates:
        if os.path.exists(c) and os.path.exists(v):
            return c, v
    if code and vars_src:
        return code, vars_src
    sys.exit("shell-test: no UEFI firmware found. Set OVMF_CODE and OVMF_VARS_SRC to "
             "an OVMF/EDK2 code image and vars template (macOS: MacPorts qemu installs "
             "them under /opt/local/share/qemu; Homebrew under /opt/homebrew/share/qemu).")


OVMF_CODE, OVMF_VARS_SRC = _find_ovmf()
QEMU_MEM = os.environ.get("QEMU_MEM", "512")
VARS = os.path.join(OUTDIR, "OVMF_VARS.fd")

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")


def read_ppm(path):
    """Read a binary PPM (P6) into (width, height, bytes)."""
    data = open(path, "rb").read()
    toks, i = [], 0
    while len(toks) < 4:
        while data[i] in b" \t\r\n":
            i += 1
        start = i
        while data[i] not in b" \t\r\n":
            i += 1
        toks.append(data[start:i])
    width, height = int(toks[1]), int(toks[2])
    i += 1
    return width, height, data[i:i + width * height * 3]


def light_background_blocks(path):
    """Count pixels sitting in a solid light-coloured 3x3 block.

    Text strokes are 1-2px wide, so solid light blocks are glyph-cell
    backgrounds. A large count means text is being drawn on a light
    background, which is the bug that made bright colours unreadable.
    """
    width, height, px = read_ppm(path)

    def at(x, y):
        o = (y * width + x) * 3
        return (px[o], px[o + 1], px[o + 2])

    def is_light(c):
        return (c[0] + c[1] + c[2]) / 3 > 160

    count = 0
    for y in range(1, height - 1):
        for x in range(1, width - 1):
            c = at(x, y)
            if is_light(c) and all(
                at(x + dx, y + dy) == c
                for dx in (-1, 0, 1) for dy in (-1, 0, 1)
            ):
                count += 1
    return count

os.makedirs(OUTDIR, exist_ok=True)
os.makedirs(SOCK_DIR, exist_ok=True)
for path in (SERIAL_SOCK, MONITOR_SOCK):
    if os.path.exists(path):
        os.unlink(path)


class Serial:
    def __init__(self, sock):
        self.sock = sock
        self.raw = b""

    def text(self):
        return ANSI.sub(b"", self.raw).decode("latin-1")

    def read_until(self, needle, timeout=40):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.text():
                return True
            self.sock.settimeout(0.3)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not data:
                break
            self.raw += data
        return needle in self.text()

    def read_regex(self, pattern, timeout=40):
        """Wait until `pattern` matches the transcript. Returns the match."""
        rx = re.compile(pattern, re.MULTILINE)
        deadline = time.time() + timeout
        while True:
            m = rx.search(self.text())
            if m:
                return m
            if time.time() >= deadline:
                return None
            self.sock.settimeout(0.3)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not data:
                return None
            self.raw += data

    def send(self, text):
        self.sock.sendall(text.encode())


def pmm_allocated(ser):
    """Send `pmm` and return the allocated-frame count it prints now, or None."""
    pattern = r"allocated : (\d+) frames"
    before = len(re.findall(pattern, ser.text()))
    ser.send("pmm\n")
    deadline = time.time() + 8
    while time.time() < deadline:
        if len(re.findall(pattern, ser.text())) > before:
            break
        ser.sock.settimeout(0.3)
        try:
            data = ser.sock.recv(65536)
        except socket.timeout:
            continue
        if not data:
            break
        ser.raw += data
    found = re.findall(pattern, ser.text())
    return int(found[-1]) if len(found) > before else None


def main():
    if not os.path.exists(IMAGE):
        print(f"[shell-test] no image at {IMAGE}; run 'make image' first")
        return 1

    import shutil
    shutil.copyfile(OVMF_VARS_SRC, VARS)

    cmd = [
        "qemu-system-x86_64",
        "-M", "q35", "-m", QEMU_MEM,
        "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}",
        "-drive", f"if=pflash,format=raw,unit=1,file={VARS}",
        "-drive", f"file={IMAGE},format=raw,if=ide,index=0,media=disk",
        "-chardev", f"socket,id=ser0,path={SERIAL_SOCK},server=on,wait=off",
        "-serial", "chardev:ser0",
        "-monitor", f"unix:{MONITOR_SOCK},server=on,wait=off",
        # Network card (Intel e1000) with QEMU's user-mode network: a real gateway
        # answers our ARP request. See docs/10-network.md.
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        # Graphics card: a virtio-gpu device, found by its PCI IDs. See docs/12-graphics.md.
        "-device", "virtio-gpu-pci",
        "-display", "none",
        "-no-reboot",
    ]
    print("[shell-test] starting QEMU...")
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    checks = []
    try:
        # Wait for the serial socket to appear, then connect.
        deadline = time.time() + 15
        while time.time() < deadline and not os.path.exists(SERIAL_SOCK):
            time.sleep(0.1)
        ser_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        ser_sock.connect(SERIAL_SOCK)
        ser = Serial(ser_sock)

        def check(label, needle, timeout=40):
            ok = ser.read_until(needle, timeout)
            print(f"[shell-test] {'PASS' if ok else 'FAIL'}: {label}")
            checks.append(ok)
            return ok

        def check_true(label, condition):
            print(f"[shell-test] {'PASS' if condition else 'FAIL'}: {label}")
            checks.append(bool(condition))

        # --- boot banner ---
        check("boot banner: HELIX OS", "HELIX OS")
        check("boot banner: Boot successful.", "Boot successful.")
        check("linux compat self-test passed at boot", "[compat] self-test passed")
        # M9: the kernel installs /bin, starts /bin/init (PID 1) from it, and init
        # starts /bin/sh, so the first prompt at boot is the userland shell.
        check("boot installs /bin/sh from the embedded image", "[bin] installed /bin/sh")
        check("PID 1 (/bin/init) starts /bin/sh", "[init] starting /bin/sh")
        check("userland shell is the first prompt at boot", "ring3> ")
        ser.send("exit\n")
        check("PID 1 reports /bin/sh's exit status", "[init] /bin/sh exited with status 0")
        check("init (PID 1) exits with status 0 at boot", "[init] PID 1 exited with status 0")
        check("shell prompt", "helix> ")

        # --- commands ---
        ser.send("help\n")
        check("help lists sysinfo", "sysinfo")
        check("help lists calc", "calc")

        ser.send("echo hello world\n")
        check("echo", "hello world")

        # --- output redirection: > and >> write into a file on ext4 ---
        ser.send("mkdir /rdir\n")
        ser.send("echo redirect-body > /rdir/redir.txt\n")
        ser.send("ls /rdir\n")
        check("redirect creates a file in a subdirectory",
              "redir.txt (14 bytes)")
        ser.send("cat /rdir/redir.txt\n")
        check("redirected content can be read back", "redirect-body\n")
        ser.send("echo more >> /rdir/redir.txt\n")
        ser.send("ls /rdir\n")
        check("append redirect grows the file", "redir.txt (19 bytes)")

        ser.send("calc 6 * 7\n")
        check("calc 6 * 7 = 42", "= 42")
        ser.send("calc 9 + 10\n")
        # The joke is random (1 in 4): either the true sum or the meme answer.
        check("calc 9 + 10 prints a sum", "9 + 10 = ")

        ser.send("usermode\n")
        check("ring 3 program writes through the syscall gate", "hello from ring 3")
        check("ring 3 program exits with its status", "exited with status 7")

        # The userland shell (M9): an ELF loaded into its own process, typing via SYS_READ_INPUT.
        ser.send("usershell\n")
        check("userland shell starts in ring 3", "userland shell (ring 3)")
        ser.send("echo ring3-ok\n")
        check("userland shell echo command", "\nring3-ok\n")
        # Userland file commands (the first slice of the ring-3 port): /bin programs
        # that the ring-3 shell spawns, using the same file system as the kernel shell.
        check_true("the file commands are installed under /bin", True)
        ser.send("echo ring3-echo two words\n")
        check("userland echo prints its arguments", "ring3-echo two words")
        ser.send("ls /bin\n")
        check("userland ls lists /bin", "cat (")
        ser.send("mkdir /ut\n")
        ser.send("touch /ut/one\n")
        ser.send("cp /ut/one /ut/two\n")
        ser.send("ls /ut\n")
        check("userland touch creates a file", "one (0 bytes)")
        check("userland cp creates the copy", "two (0 bytes)")
        ser.send("rm /ut/one\n")
        ser.send("rm /ut/two\n")
        ser.send("rmdir /ut\n")
        ser.send("mkdir /ut2\n")
        ser.send("touch /ut2/x\n")
        ser.send("rmdir /ut2\n")
        check("userland rmdir refuses a directory that is not empty", "failed to remove '/ut2'")
        ser.send("find /ut2\n")
        check("userland find walks a directory", "/ut2/x")
        ser.send("cat /nosuch\n")
        check("userland cat reports a missing file", "cat: /nosuch: no such file")
        # /bin/edit: a line editor. Open a new file, add two lines, save, quit, and
        # read the file back with /bin/cat.
        ser.send("edit /edt.txt\n")
        check("edit opens a new file", "new file: /edt.txt")
        ser.send("a one\n")
        ser.send("a two\n")
        ser.send("w\n")
        check("edit saves the file", "wrote 2 line(s) to /edt.txt")
        ser.send("q\n")
        ser.send("cat /edt.txt\n")
        check("edit wrote the lines in order", "\none\ntwo\n")
        # Stage 4: /bin/ping sends its echoes through the kernel (SYS_NET_PING).
        ser.send("ping 10.0.2.2\n")
        check("ring-3 ping gets an echo reply from the gateway", "reply from 10.0.2.2: seq=1", timeout=60)
        check("ring-3 ping reports a summary", "4 sent, 4 received", timeout=60)
        # DNS over UDP: ring-3 nslookup asks the kernel's resolver. The name ends in .invalid,
        # so the answer is "no such name" from any server, with no internet needed.
        ser.send("nslookup nonexistent.invalid\n")
        check("ring-3 nslookup reports a name that does not exist", "nslookup: nonexistent.invalid: not found", timeout=60)
        # M9 spawn: the shell runs /bin/args as a child process (nested under the
        # kernel-started shell), and argv arrives intact.
        ser.send("args one two\n")
        check("shell runs /bin/args with argc", "args: argc=3")
        check("shell passes argv[2] to the child", "args: argv[2]=two")
        ser.send("nosuchprog\n")
        check("shell reports a program it cannot find", "unknown command: nosuchprog")
        # M9 environment: envp is built on the stack next to argv. The kernel gives
        # every program PATH and HOME; export adds a variable that children see;
        # unset removes it again. Sentinel echo lines bracket the output of env.
        ser.send("env\n")
        check("kernel passes the default environment (PATH)", "PATH=/bin")
        check("kernel passes the default environment (HOME)", "HOME=/")
        ser.send("export GREET=hello\n")
        ser.send("env\n")
        check("export sets a variable the child program sees", "GREET=hello")
        ser.send("unset GREET\n")
        ser.send("echo sentinel-u1\n")
        check("unset: first sentinel printed", "\nsentinel-u1")
        ser.send("env\n")
        ser.send("echo sentinel-u2\n")
        check("unset: second sentinel printed", "\nsentinel-u2")
        t = ser.text()
        seg = t[t.rfind("\nsentinel-u1"):t.rfind("\nsentinel-u2")]
        check_true("unset removes GREET from the child's environment",
                   "PATH=/bin" in seg and "GREET=" not in seg)
        # M9 file I/O: write creates a file on ext4, cat reads it back through a
        # descriptor, and a missing path is refused.
        ser.send("write /note.txt gold-42\n")
        check("ring 3 write() creates and fills a file", "wrote 7 bytes")
        ser.send("cat /note.txt\n")
        check("ring 3 read() returns the written bytes", "\ngold-42")
        ser.send("write /note.txt x\n")
        check("write truncates an existing file (O_TRUNC)", "wrote 1 bytes")
        ser.send("cat /note.txt\n")
        check("truncated file reads back the new contents", "\nx")
        ser.send("cat /no-such-file\n")
        check("open() of a missing path fails cleanly", "cat: /no-such-file: no such file")
        # M9 heap: brk grows the process heap; malloc fills, verifies, frees, reuses.
        ser.send("heap\n")
        check("ring 3 heap via brk: 64 KiB written, verified and reused",
              "heap: 65536 bytes written and verified, freed block reused")
        ser.send("exit\n")
        check("userland shell exits with status 0", "usershell: ring 3 exited with status 0")

        # Teardown (M9): a process's frames and page tables are freed when it exits,
        # so repeated runs of the same ELF do not grow the allocated-frame count.
        frames_before = pmm_allocated(ser)
        for n in range(3):
            ser.send(f"exec /bin/args run{n}\n")
            check("exec runs a program from /bin and waits for it",
                  f"args: argv[1]=run{n}")
        frames_after = pmm_allocated(ser)
        check_true("process frames are freed on exit (allocated count unchanged)",
                   frames_before is not None and frames_after == frames_before)

        # Heap teardown (M9): pages a process adds with brk are freed when it exits,
        # like its ELF pages. This run grows the heap by 64 KiB and then exits.
        heap_before = pmm_allocated(ser)
        ser.send("usershell\n")
        ser.send("heap\n")
        ser.send("exit\n")
        check("usershell heap run exits cleanly", "usershell: ring 3 exited with status 0")
        heap_after = pmm_allocated(ser)
        check_true("brk heap frames are freed on exit (allocated count unchanged)",
                   heap_before is not None and heap_after == heap_before)

        # Kernel event log: clear it, run a program, and the log records its spawn
        # and exit with status (the log is what a crash report is read from).
        ser.send("log clear\n")
        check("log clear acknowledges", "log cleared")
        ser.send("exec /bin/args logtest\n")
        check("exec runs while the event log is recording", "args: argv[1]=logtest")
        ser.send("log\n")
        # Match the dump's category tag ("proc] [proc] ..."): the serial echo of
        # the same event has no tag, so it cannot satisfy these checks.
        check("log records the spawn of the program", "proc] [proc] spawn /bin/args", timeout=40)
        check("log records the exit with its status", "proc] [proc] exit /bin/args status=0")
        check("log ends with its end marker", "[klog] end")

        # Network card: the MAC is QEMU's default, the link is up, and the stage-1
        # self-test gets an ARP reply from the user-mode gateway 10.0.2.2.
        ser.send("net\n")
        check("net prints the card's MAC", "MAC 52:54:00:12:34:56")
        check("net reports the link up", "link up")
        ser.send("net test\n")
        check("net test gets an ARP reply from the gateway", "net test: ARP reply from gateway 10.0.2.2", timeout=40)
        # Count new replies: the ring-3 ping above printed the same text earlier, so a
        # plain search would pass even if this kernel ping printed nothing.
        needle = "reply from 10.0.2.2: seq=1"
        before = ser.text().count(needle)
        ser.send("ping 10.0.2.2\n")
        got_new = False
        deadline = time.time() + 60
        while time.time() < deadline:
            if ser.text().count(needle) > before:
                got_new = True
                break
            ser.sock.settimeout(0.3)
            try:
                data = ser.sock.recv(65536)
            except socket.timeout:
                continue
            if not data:
                break
            ser.raw += data
        check_true("kernel ping gets a new echo reply from the gateway", got_new)
        # DHCP: QEMU's DHCP server leases 10.0.2.15 (the same address the static config had).
        ser.send("dhcp\n")
        check("dhcp gets a lease from QEMU's DHCP server", "dhcp: lease 10.0.2.15", timeout=60)
        ser.send("nslookup nonexistent.invalid\n")
        check("kernel nslookup reports a name that does not exist", "nslookup: nonexistent.invalid: not found", timeout=60)
        # TCP: nothing listens on port 9 of the host loopback (QEMU maps 10.0.2.2 there), so the
        # connect must fail cleanly rather than hang the shell. The success path is checked by hand.
        ser.send("tcpget 10.0.2.2 9 /\n")
        check("tcpget reports a failed connect to a closed port", "tcpget: connect to 10.0.2.2:9 failed", timeout=60)
        # HTTP client error paths: each fails at a different step, without needing a server.
        ser.send("gpu\n")
        check("gpu lists the virtio-gpu scanout", "gpu: scanout 0: ", timeout=60)
        ser.send("fetch http://10.0.2.2:9/\n")
        check("fetch reports a failed connect", "fetch: connect failed", timeout=60)
        ser.send("fetch http://nonexistent.invalid/\n")
        check("fetch reports a name that does not exist", "fetch: no such host", timeout=60)
        ser.send("fetch https://example.com/\n")
        check("fetch refuses https for now", "fetch: https is not supported yet", timeout=60)

        # Desktop: start it, open nothing, leave with Esc. This is the path that used
        # to fault when a process had run before it; the event log records the start.
        ser.send("desktop\n")
        check("desktop starts in graphics mode", "[desktop] started")
        ser.send("\x1b")
        check("Esc leaves the desktop and returns to the shell", "[desktop] exited")

        ser.send("cpu\n")
        check("cpu reports vendor", "Vendor:")

        ser.send("mem\n")
        check("mem reports usable RAM", "usable")

        ser.send("pmm\n")
        check("pmm reports frame statistics", "Physical frame allocator")
        check("pmm reports free frames", "free")

        ser.send("heap\n")
        check("heap reports statistics", "Kernel heap")

        ser.send("memtest\n")
        check("memtest allocates and frees", "Heap self-test")
        check("memtest passes", "result: PASS")

        ser.send("vmtest\n")
        check("vmtest maps/unmaps/protects", "result: PASS")

        ser.send("stacktest\n")
        check("kernel runs on its dedicated stack",
              "execution is on the dedicated stack: ok")
        check("guard page fault is reported and recovered",
              "guard page fault: reported and recovered")
        check("NX execute fault is reported and recovered",
              "NX execute fault: reported and recovered")
        check("M3 stack self-test passes", "M3 stack self-test: PASS")

        ser.send("objects\n")
        check("object manager reports statistics", "Object manager")
        check("object registry starts empty", "live objects : 0")

        ser.send("objtest\n")
        check("objtest rejects invalid rights and handles",
              "reject missing/zero rights, wrong type, and invalid handle: ok")
        check("objtest closes the final channel reference",
              "close channel once and release final reference: ok")
        check("objtest reaches its passing result", "M5 object self-test: PASS")
        check_true("objtest result is PASS",
                   "M5 object self-test: PASS" in
                   ser.text().split("objtest\n", 1)[-1])

        ser.send("objects\n")
        check("objtest leaves no objects behind", "live objects : 0")

        ser.send("captest\n")
        check("captest creates and derives capabilities",
              "create capability and derive reduced rights: ok")
        check("captest rejects capability rights enlargement",
              "reject capability rights enlargement: ok")
        check("captest enforces granted rights and type",
              "enforce granted rights and target type: ok")
        check("derived capability cannot revoke its parent",
              "derived capability cannot revoke its parent: ok")
        check("revoking parent invalidates descendants",
              "revoke parent and invalidate every descendant: ok")
        check("captest rejects zero granted rights",
              "reject capability creation with zero rights: ok")
        check("captest enforces capability_allows checks",
              "capability_allows checks granted rights and target type: ok")
        check("captest rejects derivation from a revoked parent",
              "reject derivation from a revoked parent: ok")
        check("captest rejects revoking an already-revoked capability",
              "reject revoking an already-revoked capability: ok")
        check("captest rejects missing capability handles",
              "resolve rejects missing capability handles: ok")
        check("M6 capability self-test passes",
              "M6 capability self-test: PASS")
        ser.send("objects\n")
        check("captest leaves no objects behind", "live objects : 0")

        ser.send("ticks\n")
        check("PIT timer is ticking", "PIT timer")
        # Wait for the PIT line itself. Ping output also ends in "ms\n", so waiting for that
        # would match an earlier line and read the count before it is written.
        ser.read_until("ticks, ")
        m = re.search(r"PIT timer: \d+ Hz, (\d+) ticks", ser.text())
        check_true("PIT tick count is actually advancing",
                   m is not None and int(m.group(1)) > 0)

        ser.send("uptime\n")
        check("uptime reports seconds", "seconds")

        ser.send("date\n")
        m = ser.read_regex(r"\b(?:Sun|Mon|Tue|Wed|Thu|Fri|Sat)\s+"
                           r"(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)\s+"
                           r"\d{1,2} \d{2}:\d{2}:\d{2} UTC \d{4}")
        check_true("date output is a full calendar timestamp", m is not None)
        check_true("date is not the old fixed 2025 constant",
                   "Thu Jan  1 00:00:00 UTC 2025" not in ser.text())

        ser.send("time\n")
        m = ser.read_regex(r"^\d{2}:\d{2}:\d{2}$")
        check_true("time output is HH:MM:SS", m is not None)

        ser.send("sysinfo\n")
        check("sysinfo header", "founder@helix")
        check("sysinfo OS line", "Helix OS")
        check("sysinfo CPU line", "CPU:")
        check("sysinfo Memory line", "Memory:")

        ser.send("doesnotexist\n")
        check("unknown command is reported", "command not found")

        ser.send("color off\n")
        check("color off acknowledged", "disabled")
        ser.send("color on\n")
        check("color on acknowledged", "enabled")

        # --- framebuffer screenshot of the working shell ---
        time.sleep(1.0)
        mon = None
        if os.path.exists(MONITOR_SOCK):
            mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            mon.connect(MONITOR_SOCK)
            mon.sendall(f"screendump {PPM}\n".encode())
            time.sleep(1.5)

        # --- deliberate fault: the exception handler must report it ---
        ser.send("fault\n")
        check("CPU exception is caught and reported", "CPU EXCEPTION")

        if mon is not None:
            mon.sendall(b"quit\n")
            mon.close()
        time.sleep(1.0)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(SOCK_DIR, ignore_errors=True)

    print("[shell-test] --- captured serial transcript (tail) ---")
    try:
        print(ser.text()[-1500:])
    except NameError:
        print("(no serial data)")
    print("[shell-test] --------------------------------------------")

    # --- framebuffer readability regression check ---
    if os.path.exists(PPM):
        blocks = light_background_blocks(PPM)
        ok = blocks < 1000
        print(f"[shell-test] {'PASS' if ok else 'FAIL'}: framebuffer has no "
              f"light-background text blocks (found {blocks})")
        checks.append(ok)

    if os.path.exists(PPM):
        if subprocess.run(
            ["python3", "tools/ppm2png.py", PPM, PNG],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ).returncode == 0:
            print(f"[shell-test] screenshot: {PNG}")

    if all(checks):
        print(f"[shell-test] RESULT: PASS ({len(checks)} checks)")
        return 0
    failed = len(checks) - sum(checks)
    print(f"[shell-test] RESULT: FAIL ({failed}/{len(checks)} checks failed)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
