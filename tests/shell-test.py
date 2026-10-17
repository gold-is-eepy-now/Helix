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
import time

BUILD_DIR = os.environ.get("BUILD_DIR", "build")
IMAGE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD_DIR, "helix.img")
OUTDIR = os.path.join(BUILD_DIR, "test")
SERIAL_SOCK = os.path.join(OUTDIR, "serial.sock")
MONITOR_SOCK = os.path.join(OUTDIR, "monitor.sock")
PPM = os.path.join(OUTDIR, "shell-screen.ppm")
PNG = os.path.join(OUTDIR, "shell-screen.png")

OVMF_CODE = os.environ.get("OVMF_CODE", "/usr/share/OVMF/OVMF_CODE.fd")
OVMF_VARS_SRC = os.environ.get("OVMF_VARS_SRC", "/usr/share/OVMF/OVMF_VARS.fd")
QEMU_MEM = os.environ.get("QEMU_MEM", "512")
VARS = os.path.join(OUTDIR, "OVMF_VARS.fd")

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")

os.makedirs(OUTDIR, exist_ok=True)
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

    def send(self, text):
        self.sock.sendall(text.encode())


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

        # --- boot banner ---
        check("boot banner: HELIX OS", "HELIX OS")
        check("boot banner: Boot successful.", "Boot successful.")
        check("shell prompt", "helix> ")

        # --- commands ---
        ser.send("help\n")
        check("help lists sysinfo", "sysinfo")
        check("help lists calc", "calc")

        ser.send("echo hello world\n")
        check("echo", "hello world")

        ser.send("calc 6 * 7\n")
        check("calc 6 * 7 = 42", "= 42")

        ser.send("cpu\n")
        check("cpu reports vendor", "Vendor:")

        ser.send("mem\n")
        check("mem reports usable RAM", "usable")

        ser.send("uptime\n")
        check("uptime reports seconds", "seconds")

        ser.send("sysinfo\n")
        check("sysinfo header", "founder@helix")
        check("sysinfo OS line", "Helix OS")
        check("sysinfo CPU line", "CPU:")
        check("sysinfo Memory line", "Memory:")

        ser.send("doesnotexist\n")
        check("unknown command is reported", "command not found")

        # --- screenshot of the framebuffer console ---
        time.sleep(1.0)
        if os.path.exists(MONITOR_SOCK):
            mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            mon.connect(MONITOR_SOCK)
            mon.sendall(f"screendump {PPM}\n".encode())
            time.sleep(1.5)
            mon.sendall(b"quit\n")
            mon.close()
        time.sleep(1.0)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    print("[shell-test] --- captured serial transcript (tail) ---")
    try:
        print(ser.text()[-1500:])
    except NameError:
        print("(no serial data)")
    print("[shell-test] --------------------------------------------")

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
