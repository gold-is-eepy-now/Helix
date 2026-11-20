#!/usr/bin/env python3
"""Photographs every desktop wallpaper under QEMU.

Boots a copy of the test image, signs in as a new account, opens Settings from the Start menu,
and for each wallpaper presses w (next wallpaper) and takes a QEMU screendump of the desktop. Writes one PNG per wallpaper and a labelled contact sheet.

The wallpaper names are read from kernel/src/desktop.c, so the photo labels follow the table.
Keys go in over the serial console, the same way tests/shell-test.py drives the desktop.

Usage: tests/wallpaper-shots.py [IMAGE] [OUTDIR]
  IMAGE   default build/helix.img (copied first; the original is not changed)
  OUTDIR  default build/wallpapers
Needs Pillow (python3 -m pip install pillow). Accelerator: whatever QEMU uses; this script
does not choose one. Say which one when reporting results. """
import importlib.util
import os
import random
import re
import shutil
import socket
import string
import subprocess
import sys
import time

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Reuse the harness's paths, QEMU memory size and serial helper. shell-test.py reads argv at
# import time, so set argv to what it expects before loading it.
_argv = sys.argv
sys.argv = [_argv[0]]
_spec = importlib.util.spec_from_file_location("shelltest", os.path.join(HERE, "shell-test.py"))
st = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(st)
sys.argv = _argv

IMAGE_SRC = _argv[1] if len(_argv) > 1 else os.path.join(ROOT, "build", "helix.img")
OUTDIR = _argv[2] if len(_argv) > 2 else os.path.join(ROOT, "build", "wallpapers")
WORK = os.path.join(OUTDIR, "work")
SETTLE = 8                              # seconds to let the desktop repaint before each shot


def wallpaper_names():
    """The names in g_wallpapers, in table order, read from the desktop source."""
    src = open(os.path.join(ROOT, "kernel", "src", "desktop.c"), encoding="utf-8").read()
    block = src[src.index("static const wallpaper_t g_wallpapers["):]
    block = block[:block.index("};")]
    return re.findall(r'\{ "([^"]+)",', block)


def slug(name):
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def to_png(ppm, png):
    Image.open(ppm).convert("RGB").save(png)


def contact_sheet(pngs, names, path, cols=4, thumb=(320, 200)):
    rows = (len(pngs) + cols - 1) // cols
    label_h = 22
    sheet = Image.new("RGB", (cols * (thumb[0] + 12) + 12, rows * (thumb[1] + label_h + 12) + 12),
                      (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for i, (png, name) in enumerate(zip(pngs, names)):
        x = 12 + (i % cols) * (thumb[0] + 12)
        y = 12 + (i // cols) * (thumb[1] + label_h + 12)
        im = Image.open(png).convert("RGB").resize(thumb)
        sheet.paste(im, (x, y))
        draw.text((x, y + thumb[1] + 4), name, fill=(235, 235, 235))
    sheet.save(path)


def main():
    if not os.path.exists(IMAGE_SRC):
        print(f"wallpaper-shots: no image at {IMAGE_SRC}; build it first (make image CC=gcc)")
        return 2
    os.makedirs(WORK, exist_ok=True)
    image = os.path.join(WORK, "helix-shots.img")
    shutil.copyfile(IMAGE_SRC, image)
    varsfd = os.path.join(WORK, "OVMF_VARS.fd")
    shutil.copyfile(st.OVMF_VARS_SRC, varsfd)

    names = wallpaper_names()
    print(f"wallpaper-shots: {len(names)} wallpapers: {', '.join(names)}")

    cmd = [
        "qemu-system-x86_64", "-M", "q35", "-m", st.QEMU_MEM, "-cpu", "qemu64,+rdrand",
        "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={st.OVMF_CODE}",
        "-drive", f"if=pflash,format=raw,unit=1,file={varsfd}",
        "-drive", f"file={image},format=raw,if=ide,index=0,media=disk",
        "-chardev", f"socket,id=ser0,path={st.SERIAL_SOCK},server=on,wait=off",
        "-serial", "chardev:ser0",
        "-monitor", f"unix:{st.MONITOR_SOCK},server=on,wait=off",
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        # No virtio-gpu here, on purpose. With virtio-gpu present the desktop draws to a backing
        # buffer and flushes it to the scanout, and QEMU's screendump keeps showing the boot log.
        # Without it the desktop draws straight into the framebuffer, which screendump captures.
        "-display", "none", "-no-reboot",
    ]
    for path in (st.SERIAL_SOCK, st.MONITOR_SOCK):
        if os.path.exists(path):
            os.remove(path)
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ser = None
    try:
        deadline = time.time() + 20
        while time.time() < deadline and not os.path.exists(st.SERIAL_SOCK):
            time.sleep(0.1)
        ser_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        ser_sock.connect(st.SERIAL_SOCK)
        ser = st.Serial(ser_sock)
        mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        deadline = time.time() + 20
        while True:
            try:
                mon.connect(st.MONITOR_SOCK)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise
                time.sleep(0.1)

        def shot(ppm_name):
            ppm = os.path.join(WORK, ppm_name)
            # The desktop repaints the wallpaper after each change. Under software emulation that
            # takes seconds, so wait for it before the screendump.
            time.sleep(SETTLE)
            mon.sendall(f"screendump {ppm}\n".encode())
            time.sleep(1.5)
            return ppm

        # Sign in: the account is new, so the password is known to this script.
        if not ser.read_until("[ui] sign-in", 150):
            print("wallpaper-shots: the sign-in screen did not appear")
            return 1
        ser.send("\x14")                                   # Ctrl+T opens the TUI shell
        ser.read_until("helix> ", 60)
        user = "shot" + "".join(random.choice(string.ascii_lowercase) for _ in range(5))
        pw = "photo" + str(random.randint(1000, 9999))
        ser.send(f"mkusr {user} {pw}\n")
        if not ser.read_until(f"mkusr: created '{user}'", 30):
            print("wallpaper-shots: could not create the photo account")
            return 1
        ser.read_until("helix> ", 30)
        ser.send("desktop\n")
        time.sleep(1.0)
        ser.send(f"{user}\t")
        ser.send(f"{pw}\n")
        if not ser.read_until("[desktop] started", 150):
            print("wallpaper-shots: the desktop did not start after sign-in")
            return 1
        time.sleep(1.0)

        def open_settings():
            ser.send("s")                                  # Start menu
            time.sleep(0.6)
            ser.send("settings")                           # the search box filters the list
            time.sleep(0.6)
            ser.send("\n")                                 # launch the first match
            time.sleep(1.0)

        # Settings stays open and w cycles it. Closing and reopening it from the Start menu was
        # tried first, and after a few rounds the menu's search kept the keys and the wallpaper
        # stopped changing. The Settings window shows in one corner of each photo; its button
        # names the wallpaper.
        open_settings()
        time.sleep(1.0)
        pngs, shown = [], []
        for i, name in enumerate(names):
            if i > 0:
                ser.send("w")                              # next wallpaper
                time.sleep(0.6)
            ppm = shot(f"{i:02d}.ppm")
            png = os.path.join(OUTDIR, f"{i:02d}-{slug(name)}.png")
            if not os.path.exists(ppm):
                print(f"wallpaper-shots: no screendump for {name}")
                return 1
            to_png(ppm, png)
            pngs.append(png)
            shown.append(name)
            print(f"wallpaper-shots: {name} -> {png}")

        sheet = os.path.join(OUTDIR, "contact-sheet.png")
        contact_sheet(pngs, shown, sheet)
        print(f"wallpaper-shots: contact sheet -> {sheet}")
        return 0
    finally:
        if ser is not None:                                # the serial transcript, for diagnosis
            with open(os.path.join(WORK, "serial.txt"), "w", encoding="latin-1") as f:
                f.write(ser.text())
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
