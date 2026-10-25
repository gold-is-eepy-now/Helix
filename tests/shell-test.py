#!/usr/bin/env python3
"""shell-test.py — boot Helix and drive the interactive shell.

Unlike boot-test.sh (which only checks the banner), this harness talks to the
kernel's serial port, types real commands, and checks the responses. It uses
stdio pipes so it works in restricted environments that cannot create sockets.

Exit status is 0 only if every check passes.
"""
import os
import json
import re
import socket
import select
import subprocess
import sys
import time

BUILD_DIR = os.environ.get("BUILD_DIR", "build")
IMAGE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD_DIR, "helix.img")
OUTDIR = os.path.join(BUILD_DIR, "test")
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
        ("/opt/homebrew/share/qemu/edk2-x86_64-code.fd",
         "/opt/homebrew/share/qemu/edk2-i386-vars.fd"),
        ("/opt/local/share/qemu/edk2-x86_64-code.fd",
         "/opt/local/share/qemu/edk2-i386-vars.fd"),
        ("/usr/local/share/qemu/edk2-x86_64-code.fd",
         "/usr/local/share/qemu/edk2-i386-vars.fd"),
        ("/usr/share/qemu/edk2-x86_64-code.fd",
         "/usr/share/qemu/edk2-i386-vars.fd"),
        ("/usr/share/edk2/x86_64/OVMF_CODE.fd",
         "/usr/share/edk2/x86_64/OVMF_VARS.fd"),
    ]
    for c, v in candidates:
        if os.path.exists(c) and os.path.exists(v):
            return c, v
    return code or candidates[0][0], vars_src or candidates[0][1]


OVMF_CODE, OVMF_VARS_SRC = _find_ovmf()
QEMU_MEM = os.environ.get("QEMU_MEM", "512")
QEMU_ACCEL = os.environ.get("QEMU_ACCEL")
USB_TEST_CONTROLLER = os.environ.get("USB_TEST_CONTROLLER")
USB_TEST_IMAGE = os.environ.get("USB_TEST_IMAGE")
USB_TEST_HUB = os.environ.get("USB_TEST_HUB") == "1"
USB_TEST_FORMAT = os.environ.get("USB_TEST_FORMAT") == "1"
USB_TEST_FAT32_WRITE = os.environ.get("USB_TEST_FAT32_WRITE") == "1"
USB_TEST_FAT32_DELETE = os.environ.get("USB_TEST_FAT32_DELETE") == "1"
VARS = os.path.join(OUTDIR, "OVMF_VARS.fd")
QMP_SOCKET = os.path.join(OUTDIR, "qmp.sock")

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
for path in (PPM, PNG):
    if os.path.exists(path):
        os.unlink(path)


class Serial:
    def __init__(self, process):
        self.process = process
        self.raw = b""

    def text(self):
        return ANSI.sub(b"", self.raw).decode("latin-1")

    def read_until(self, needle, timeout=40):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.text():
                return True
            ready, _, _ = select.select([self.process.stdout], [], [], 0.3)
            if not ready:
                continue
            data = os.read(self.process.stdout.fileno(), 65536)
            if not data:
                break
            self.raw += data
        return needle in self.text()

    def send(self, text):
        self.process.stdin.write(text.encode())
        self.process.stdin.flush()


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
        "-serial", "stdio",
        "-monitor", "none",
        "-display", "none",
        "-no-reboot",
    ]
    if QEMU_ACCEL:
        cmd[3:3] = ["-accel", QEMU_ACCEL]
    if USB_TEST_CONTROLLER:
        if USB_TEST_CONTROLLER == "ehci":
            cmd.extend(["-device", "usb-ehci,id=helix-ehci"])
            usb_bus = "helix-ehci.0"
        elif USB_TEST_CONTROLLER == "ich9-ehci":
            # Match VirtualBox's USB 2.0 topology more closely: an ICH9 EHCI
            # controller with its three UHCI companions sharing six ports.
            cmd.extend([
                "-device", "ich9-usb-ehci1,id=helix-ehci,multifunction=on",
                "-device", "ich9-usb-uhci1,masterbus=helix-ehci.0,firstport=0",
                "-device", "ich9-usb-uhci2,masterbus=helix-ehci.0,firstport=2",
                "-device", "ich9-usb-uhci3,masterbus=helix-ehci.0,firstport=4",
            ])
            usb_bus = "helix-ehci.0"
        elif USB_TEST_CONTROLLER == "xhci":
            cmd.extend(["-device", "qemu-xhci,id=helix-xhci"])
            usb_bus = "helix-xhci.0"
        else:
            print("[shell-test] USB_TEST_CONTROLLER must be ehci, ich9-ehci, or xhci")
            return 1
        if USB_TEST_HUB:
            try:
                os.unlink(QMP_SOCKET)
            except FileNotFoundError:
                pass
            cmd.extend(["-qmp", f"unix:{QMP_SOCKET},server=on,wait=off"])
            cmd.extend(["-device", f"usb-hub,id=helix-hub,bus={usb_bus},port=1"])
            # A standard HID keyboard behind the hub mirrors the topology of
            # the Air's internal USB 2.0 hub.
            cmd.extend(["-device", f"usb-kbd,bus={usb_bus},port=1.1"])
        if USB_TEST_IMAGE:
            storage_port = ",port=1.1" if USB_TEST_HUB else ""
            cmd.extend(["-drive", f"if=none,id=helix-usb-drive,file={USB_TEST_IMAGE},format=raw",
                        "-device", f"usb-storage,bus={usb_bus}{storage_port},drive=helix-usb-drive"])
    print("[shell-test] starting QEMU...")
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, bufsize=0)

    checks = []
    try:
        ser = Serial(proc)

        def check(label, needle, timeout=40):
            ok = ser.read_until(needle, timeout)
            print(f"[shell-test] {'PASS' if ok else 'FAIL'}: {label}")
            checks.append(ok)
            return ok

        def check_true(label, condition):
            print(f"[shell-test] {'PASS' if condition else 'FAIL'}: {label}")
            checks.append(bool(condition))

        # --- boot banner ---
        prompt_ok = ser.read_until("user@helix:~$ ")
        check_true("boot banner: HELIX OS", "HELIX OS" in ser.text())
        check_true("boot banner: Boot successful.", "Boot successful." in ser.text())
        check_true("shell prompt", prompt_ok)

        # --- native user-shell commands and syscall-backed devices ---
        ser.send("help\n")
        check("help documents sysinfo", "sysinfo")
        check("help documents file navigation", "ls, cd DIR|")

        ser.send("echo hello world\n")
        check("user shell echo", "hello world")

        ser.send("calc 6 * 7\n")
        check("user shell calculator", "42")

        ser.send("version\n")
        check("version syscall reports the current build", "0.21.0-dev.20261025")

        ser.send("sysinfo\n")
        check("sysinfo syscall reaches the kernel", "Helix kernel")

        ser.send("drivers\n")
        check("driver inventory syscall responds", "Registered device drivers")

        if USB_TEST_IMAGE:
            ser.send("usb\n")
            check("USB controller initializes", "USB host controllers on PCI bus")
            check("USB mass-storage transport is ready", "USB mass storage: BOT transport ready")
            ser.send("usb read 0\n")
            check("USB sector read succeeds", "sector read; first 16 bytes:")
        elif USB_TEST_HUB:
            ser.send("usb\n")
            check("USB controller initializes", "USB host controllers on PCI bus")
            check("USB hub interface is identified", "class=0x0000000000000009")
            check("USB hub descriptor and ports are read", "USB hub descriptor: ports=")
            check("downstream USB keyboard is connected",
                  "hub port 1 status=")
            check("downstream port speed is reported", "speed=full connected")
            check("downstream device is enumerated", "USB hub child: descriptors read",
                  timeout=3)
            check("downstream device matches HID boot keyboard",
                  "USB HID boot keyboard bound: interrupt-IN polling active", timeout=3)
            def qmp_key(qcode):
                for down in (True, False):
                    message = {
                        "execute": "input-send-event",
                        "arguments": {"events": [{"type": "key", "data": {
                            "down": down,
                            "key": {"type": "qcode", "data": qcode}
                        }}]}
                    }
                    qmp.sendall((json.dumps(message) + "\n").encode())
                    response = b""
                    while b"\n" not in response:
                        chunk = qmp.recv(4096)
                        if not chunk:
                            raise RuntimeError("QMP disconnected")
                        response += chunk
                    if b'"error"' in response:
                        raise RuntimeError(response.decode(errors="replace"))
                # Allow multiple interrupt intervals for QEMU's full-speed
                # HID device and the kernel's polled event-loop service.
                time.sleep(0.1)

            qmp = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            deadline = time.time() + 10
            while True:
                try:
                    qmp.connect(QMP_SOCKET)
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    if time.time() >= deadline:
                        raise RuntimeError("QMP socket did not become available")
                    time.sleep(0.05)
            qmp.settimeout(3)
            greeting = b""
            while b'"QMP"' not in greeting:
                greeting += qmp.recv(4096)
            qmp.sendall(b'{"execute":"qmp_capabilities"}\n')
            response = qmp.recv(4096)
            if b'"error"' in response:
                raise RuntimeError(response.decode(errors="replace"))
            for key in ("e", "c", "h", "o", "spc", "u", "s", "b", "k",
                        "e", "y", "o", "k", "ret"):
                qmp_key(key)
            check("USB HID keyboard types into the user shell",
                  "usbkeyok", timeout=5)
            qmp.close()
            if USB_TEST_FORMAT:
                ser.send("hfs format usb-msd\n")
                check("EHCI block writes format a blank test disk",
                      "HFS v1 formatted blank disk usb-msd")
                ser.send("hfs ls\n")
                check("formatted EHCI volume mounts through HFS", "HFS v1 root directory")
                ser.send("usb read 0\n")
                check("EHCI write is readable through the storage service",
                      "48 45 4c 49 58 46 53 31")
            if USB_TEST_FAT32_WRITE:
                ser.send("usb put TEST.TXT updated by Helix\n")
                check("FAT32 write creates or replaces a root file",
                      "FAT32 file saved.")
                ser.send("usb cat TEST.TXT\n")
                check("FAT32 read returns the saved content",
                      "updated by Helix")
            if USB_TEST_FAT32_DELETE:
                ser.send("usb rm REMOVE.TXT\n")
                check("FAT32 delete removes a root file", "FAT32 file removed.")
                ser.send("usb cat REMOVE.TXT\n")
                check("deleted FAT32 file is no longer found",
                      "FAT32 file not found in the root directory.")

        ser.send("uart z\n")
        check("capability-gated UART device control", "UART device control sent")

        ser.send("command-does-not-exist\n")
        check("unknown user command is reported", "command not found")

        time.sleep(0.2)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    print("[shell-test] --- captured serial transcript (tail) ---")
    try:
        transcript = ser.text()
        with open(os.path.join(OUTDIR, "serial.log"), "w", encoding="latin-1") as log:
            log.write(transcript)
        print(transcript[-6000:])
    except NameError:
        print("(no serial data)")
    print("[shell-test] --------------------------------------------")

    if USB_TEST_FORMAT and USB_TEST_IMAGE:
        with open(USB_TEST_IMAGE, "rb") as disk:
            formatted = disk.read(8) == b"HELIXFS1"
        print(f"[shell-test] {'PASS' if formatted else 'FAIL'}: host readback verifies EHCI HFS write")
        checks.append(formatted)

    if all(checks):
        print(f"[shell-test] RESULT: PASS ({len(checks)} checks)")
        return 0
    failed = len(checks) - sum(checks)
    print(f"[shell-test] RESULT: FAIL ({failed}/{len(checks)} checks failed)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
