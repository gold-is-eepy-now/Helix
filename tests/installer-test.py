#!/usr/bin/env python3
"""Boot the installer ISO, install to a new SATA qcow2, and verify HFS/GPT."""
import os
import re
import selectors
import shutil
import subprocess
import sys
import time

BUILD_DIR = os.environ.get("BUILD_DIR", "build")
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD_DIR, "helix.iso")
TARGET = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    BUILD_DIR, "test", "installer-target.qcow2")
RAW = os.path.join(BUILD_DIR, "test", "installer-target.raw")
VARS = os.path.join(BUILD_DIR, "test", "installer-vars.fd")
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
QEMU_ACCEL = os.environ.get("QEMU_ACCEL")


def firmware_paths():
    code, variables = os.environ.get("OVMF_CODE"), os.environ.get("OVMF_VARS_SRC")
    if code and variables:
        return code, variables
    pairs = [
        ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd"),
        ("/opt/homebrew/share/qemu/edk2-x86_64-code.fd",
         "/opt/homebrew/share/qemu/edk2-i386-vars.fd"),
        ("/opt/local/share/qemu/edk2-x86_64-code.fd",
         "/opt/local/share/qemu/edk2-i386-vars.fd"),
    ]
    return next(((c, v) for c, v in pairs if os.path.isfile(c) and os.path.isfile(v)),
                (code or pairs[0][0], variables or pairs[0][1]))


def main():
    code, vars_source = firmware_paths()
    if not os.path.isfile(ISO):
        raise RuntimeError(f"ISO not found: {ISO}")
    if not os.path.isfile(code) or not os.path.isfile(vars_source):
        raise RuntimeError("UEFI firmware missing; set OVMF_CODE and OVMF_VARS_SRC")
    if os.path.exists(TARGET) or os.path.exists(RAW):
        raise RuntimeError("refusing to overwrite an existing installer test target")
    os.makedirs(os.path.dirname(TARGET), exist_ok=True)
    shutil.copyfile(vars_source, VARS)
    subprocess.run(["qemu-img", "create", "-f", "qcow2", TARGET, "128M"],
                   check=True, stdout=subprocess.DEVNULL)

    cmd = [QEMU, "-M", "q35", "-m", "512"]
    if QEMU_ACCEL:
        cmd.extend(["-accel", QEMU_ACCEL])
    cmd.extend([
        "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
        "-drive", f"if=pflash,format=raw,unit=1,file={VARS}",
        "-cdrom", ISO, "-boot", "order=d",
        "-drive", f"file={TARGET},format=qcow2,if=ide,index=1,media=disk",
        "-serial", "stdio", "-monitor", "none", "-display", "none", "-no-reboot",
    ])
    print("[installer-test] booting ISO with a blank 128 MiB SATA qcow2 target")
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, bufsize=0)
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    transcript = bytearray()

    def wait_for(needle, timeout=240):
        target = needle.encode()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if target in transcript:
                return True
            if proc.poll() is not None:
                break
            events = selector.select(0.25)
            for key, _ in events:
                chunk = os.read(key.fileobj.fileno(), 65536)
                if not chunk:
                    break
                transcript.extend(chunk)
                sys.stdout.write(chunk.decode("latin-1", errors="replace"))
                sys.stdout.flush()
        return target in transcript

    def send(line):
        proc.stdin.write((line + "\n").encode())
        proc.stdin.flush()

    try:
        if not wait_for("user@helix:~$ "):
            raise RuntimeError("ISO did not reach the native user shell")
        send("installer")
        if not wait_for("Enter the exact target device name"):
            raise RuntimeError("installer TUI did not start")
        if b"sata0" not in transcript:
            raise RuntimeError("installer did not detect the SATA target")
        print("\n[installer-test] target sata0 is listed")
        send("sata0")
        if not wait_for("Type INSTALL to overwrite sata0:"):
            raise RuntimeError("installer did not request confirmation for sata0")
        send("INSTALL")
        if not wait_for("Installation complete."):
            raise RuntimeError("installer did not report completion")
        print("\n[installer-test] installation completed")
    finally:
        selector.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    subprocess.run(["qemu-img", "convert", "-O", "raw", TARGET, RAW], check=True)
    verify = subprocess.run(["sgdisk", "-v", RAW], check=True,
                            capture_output=True, text=True)
    if "No problems found" not in verify.stdout + verify.stderr:
        raise RuntimeError("installed GPT verification did not pass")
    info = subprocess.run(["sgdisk", "-i", "2", RAW], check=True,
                          capture_output=True, text=True)
    match = re.search(r"First sector:\s*(\d+)", info.stdout + info.stderr)
    if not match:
        raise RuntimeError("could not locate installed HFS partition")
    with open(RAW, "rb") as disk:
        disk.seek(int(match.group(1)) * 512)
        if disk.read(8) != b"HELIXFS1":
            raise RuntimeError("installed HFS partition signature is missing")
    print("[installer-test] PASS: blank SATA disk installed; GPT and HFS verified")
    print(f"[installer-test] disposable target: {TARGET}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"[installer-test] FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
