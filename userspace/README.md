# userspace/

Native Helix user-space programs and libraries (planned).

Programs here target the **native object API**: they receive handles to objects
and hold explicit capabilities, rather than Unix file descriptors and a root
user. The first inhabitant will be `init`, the first user-space process, once
the ELF loader and native system-call ABI exist (milestone M9).

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

Nothing is implemented here yet for native Helix API. See [../ROADMAP.md](../ROADMAP.md).
