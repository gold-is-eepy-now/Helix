#!/usr/bin/env python3
"""usb-test.py — boot Helix with USB topologies and check the USB stack end to end.

Each scenario builds a QEMU machine that matches a real chipset layout:

  * an EHCI controller with three UHCI companions (Intel ICH9/PCH style, the
    layout of the 2010 iMac 11,2), so every full-speed device goes through UHCI;
  * a USB hub on the root port, with the keyboard or mouse behind the hub;
  * a keyboard directly on a root port (no hub).

The keyboard scenarios press keys with the monitor's sendkey and then ask the
kernel, with the `usb` command, how many HID reports the USB keyboard delivered.
That count is the evidence: QEMU's sendkey also reaches the built-in PS/2
keyboard (the i8042 is present even with -nodefaults), so the shell receiving
text proves nothing about USB. The report count only moves when a USB report
arrives. The no-keyboard control checks the opposite: no USB keyboard appears.

Exit status is 0 only if every scenario passes.
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

BUILD_DIR = os.environ.get("BUILD_DIR", "build")
IMAGE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD_DIR, "helix.img")
OUTDIR = os.path.join(BUILD_DIR, "test-usb")
def _socket_dir(outdir):
    """AF_UNIX socket paths are limited to about 108 bytes. A deep build directory
    can exceed that, so fall back to a short private directory in that case."""
    if len(os.path.join(outdir, "monitor.sock")) < 100:
        return outdir
    return tempfile.mkdtemp(prefix="helix-sock-")

SOCK_DIR = _socket_dir(OUTDIR)
SERIAL_SOCK = os.path.join(SOCK_DIR, "serial.sock")
MONITOR_SOCK = os.path.join(SOCK_DIR, "monitor.sock")

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")


def find_ovmf():
    code = os.environ.get("OVMF_CODE")
    vars_src = os.environ.get("OVMF_VARS_SRC")
    if code and vars_src:
        return code, vars_src
    for c, v in (
        ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd"),
        ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
        ("/usr/share/edk2/x86_64/OVMF_CODE.fd", "/usr/share/edk2/x86_64/OVMF_VARS.fd"),
    ):
        if os.path.exists(c) and os.path.exists(v):
            return c, v
    sys.exit("usb-test: no OVMF firmware found (set OVMF_CODE and OVMF_VARS_SRC)")


OVMF_CODE, OVMF_VARS_SRC = find_ovmf()

# The chipset layout, shared by every scenario: two EHCI controllers (00:1d.7 and
# 00:1a.7), each with three UHCI companions, as on the 2010 iMac 11,2 (Ibex Peak).
CONTROLLERS = [
    "-device", "ich9-usb-ehci1,id=ehci",
    "-device", "ich9-usb-uhci1,masterbus=ehci.0,firstport=0,addr=1d.0,multifunction=on",
    "-device", "ich9-usb-uhci2,masterbus=ehci.0,firstport=2,addr=1d.1",
    "-device", "ich9-usb-uhci3,masterbus=ehci.0,firstport=4,addr=1d.2",
    "-device", "ich9-usb-ehci2,id=ehci2",
    "-device", "ich9-usb-uhci4,masterbus=ehci2.0,firstport=0,addr=1a.0,multifunction=on",
    "-device", "ich9-usb-uhci5,masterbus=ehci2.0,firstport=2,addr=1a.1",
    "-device", "ich9-usb-uhci6,masterbus=ehci2.0,firstport=4,addr=1a.2",
]

SCENARIOS = [
    {
        "name": "keyboard behind a hub (iMac 11,2 layout)",
        "devices": ["-device", "usb-hub,id=hub1,bus=ehci.0,port=1",
                    "-device", "usb-kbd,bus=ehci.0,port=1.1"],
        "expect": ["[usb] ehci at", "[usb] uhci at", "[usb] hub at address 1",
                   "[usb] address 2 interface 0: HID keyboard"],
        "type": True,
        "keyboard": True,
    },
    {
        "name": "keyboard on a root port",
        "devices": ["-device", "usb-kbd,bus=ehci.0,port=2"],
        "expect": ["[usb] address 1 interface 0: HID keyboard"],
        "type": True,
        "keyboard": True,
    },
    {
        "name": "mouse behind a hub",
        "devices": ["-device", "usb-hub,id=hub1,bus=ehci.0,port=1",
                    "-device", "usb-mouse,bus=ehci.0,port=1.2"],
        "expect": ["[usb] hub at address 1", "[usb] address 2 interface 0: HID mouse"],
        "type": False,
    },
    {
        # Negative control: no USB keyboard anywhere; no keyboard may be reported.
        "name": "control: hub with no keyboard",
        "devices": ["-device", "usb-hub,id=hub1,bus=ehci.0,port=1"],
        "expect": ["[usb] hub at address 1"],
        "type": True,
        "keyboard": False,
    },
]


def make_fat_image(path):
    """A 34 MiB FAT32 volume for the mass-storage scenarios. Returns the 100000-byte test
    file's contents, or None (with a printed note) when dosfstools or mtools is missing."""
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1", LANG="C.UTF-8", LC_ALL="C.UTF-8",
               PATH=os.environ.get("PATH", "") + ":/usr/sbin:/sbin")
    for tool in ("mkfs.fat", "mcopy"):
        if shutil.which(tool, path=env["PATH"]) is None:
            print(f"[usb-test] SKIP mass-storage scenarios: {tool} not found (install dosfstools and mtools)")
            return None
    big = bytes((i * 7 + (i // 512) * 13 + 5) % 251 for i in range(100000))
    hello = os.path.join(OUTDIR, "usb-hello.txt")
    bigf = os.path.join(OUTDIR, "usb-big.bin")
    open(hello, "wb").write(b"Hello from USB!\n")
    open(bigf, "wb").write(big)
    with open(path, "wb") as f:
        f.truncate(34 * 1024 * 1024)
    def run(*args):
        subprocess.run(args, check=True, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run("mkfs.fat", "-F", "32", "-s", "1", "-n", "USBSTICK", path)
    run("mcopy", "-i", path, hello, "::/HELLO.TXT")
    run("mcopy", "-i", path, bigf, "::/BIG.BIN")
    return big

# QEMU key names (qcodes) for "echo kbd-ok" followed by Enter.
TYPED_KEYS = ["e", "c", "h", "o", "spc", "k", "b", "d", "minus", "o", "k", "ret"]
NEEDLE = "\nkbd-ok"


class Serial:
    def __init__(self, sock):
        self.sock = sock
        self.raw = b""

    def text(self):
        return ANSI.sub(b"", self.raw).decode("latin-1")

    def read_until(self, needle, timeout):
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

    def send(self, text):
        self.sock.sendall(text.encode())

    def pump(self, seconds):
        """Keep reading for a while, without waiting for any particular text."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.sock.settimeout(0.2)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not data:
                break
            self.raw += data


def wait_socket(path, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline and not os.path.exists(path):
        time.sleep(0.1)
    return os.path.exists(path)


def run_scenario(index, scenario):
    print(f"[usb-test] scenario {index + 1}: {scenario['name']}")
    for path in (SERIAL_SOCK, MONITOR_SOCK):
        if os.path.exists(path):
            os.unlink(path)
    vars_copy = os.path.join(OUTDIR, f"OVMF_VARS_{index}.fd")
    shutil.copyfile(OVMF_VARS_SRC, vars_copy)
    cmd = [
        "qemu-system-x86_64", "-M", "q35", "-nodefaults", "-m", "512",
        "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}",
        "-drive", f"if=pflash,format=raw,unit=1,file={vars_copy}",
        "-drive", f"file={IMAGE},format=raw,if=ide,index=0,media=disk",
        *CONTROLLERS, *scenario["devices"],
        "-chardev", f"socket,id=ser0,path={SERIAL_SOCK},server=on,wait=off",
        "-serial", "chardev:ser0",
        "-monitor", f"unix:{MONITOR_SOCK},server=on,wait=off",
        "-display", "none", "-no-reboot",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    results = []
    try:
        if not wait_socket(SERIAL_SOCK):
            return [("serial socket appeared", False)]
        ser = Serial(_connect(SERIAL_SOCK))

        def check(label, ok):
            print(f"[usb-test]   {'PASS' if ok else 'FAIL'}: {label}")
            results.append((label, bool(ok)))

        # This test boots with -nodefaults: no framebuffer, so there is no sign-in
        # screen. PID 1 starts /bin/sh first, so the boot prompt is the userland shell.
        # Exit it over serial to reach the kernel shell, which the rest of this test uses.
        check("userland shell prompt reached at boot", ser.read_until("ring3> ", 60))
        ser.send("exit\n")
        check("kernel reached the shell prompt", ser.read_until("helix> ", 60))
        for needle in scenario["expect"]:
            check(f"log: {needle}", ser.read_until(needle, 5))
        for command, needle, wait in scenario.get("commands", []):
            ser.send(command + "\n")
            check(f"{command!r} prints {needle!r}", ser.read_until(needle, wait))
        if scenario["type"]:
            mon = _connect(MONITOR_SOCK)
            time.sleep(0.5)
            for key in TYPED_KEYS:
                mon.sendall(f"sendkey {key}\n".encode())
                time.sleep(0.12)
            mon.close()
            ser.pump(2)
            ser.send("usb\n")
            ser.pump(2)
            reports = re.findall(r"interface \d+ HID keyboard: reports (\d+)", ser.text())
            if scenario["keyboard"]:
                check("USB keyboard delivered reports for the typed keys",
                      bool(reports) and int(reports[-1]) > 0)
            else:
                check("no USB keyboard reported without one attached", not reports)
    finally:
        proc.kill()
        proc.wait()
    return results


def _connect(path):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(path)
    return sock


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    all_results = []
    scenarios = list(SCENARIOS)
    stick = os.path.join(OUTDIR, "usb-stick.img")
    big = make_fat_image(stick)
    if big is not None:
        total = sum(big) & 0xFFFFFFFF
        drive = ["-drive", f"if=none,id=stick,file={stick},format=raw"]
        commands = [
            ("disk", "usb0: USB", 30),
            ("disk", "usb0: sector 0 signature 55aa", 30),
            ("fat usb0 info", "USBSTICK", 30),
            ("fat usb0 ls /", "f 16 HELLO.TXT", 30),
            ("fat usb0 cat /hello.txt", "Hello from USB!", 30),
            ("fat usb0 sum /big.bin", f"fat: /big.bin 100000 bytes, sum {total}", 240),
        ]
        scenarios.append({
            "name": "USB mass storage on a root port (FAT32 stick)",
            "devices": drive + ["-device", "usb-storage,bus=ehci.0,port=1,drive=stick"],
            "expect": ["[usb-storage] usb0:"],
            "type": False,
            "commands": commands,
        })
        scenarios.append({
            "name": "USB mass storage behind a hub",
            "devices": drive + ["-device", "usb-hub,id=hub1,bus=ehci.0,port=1",
                                "-device", "usb-storage,bus=ehci.0,port=1.1,drive=stick"],
            "expect": ["[usb] hub at address 1", "[usb-storage] usb0:"],
            "type": False,
            "commands": commands[:3] + [commands[5]],
        })
    for i, scenario in enumerate(scenarios):
        all_results += run_scenario(i, scenario)
    failed = [label for label, ok in all_results if not ok]
    print(f"[usb-test] {len(all_results) - len(failed)}/{len(all_results)} checks passed")
    if failed:
        print("[usb-test] RESULT: FAIL")
        for label in failed:
            print(f"[usb-test]   failed: {label}")
        return 1
    print("[usb-test] RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
