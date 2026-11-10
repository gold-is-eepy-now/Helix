# 10 — Network stack

Status: **in progress** (M12). Stages 1–4 and 6–7 are built and QEMU-tested; stage 5 is partly checked (see there). Stages below are marked as they are built and
tested. Nothing here is implemented until its stage says so.

## Goal

Send and receive IPv4 packets on a real network interface, starting with ICMP
echo ("ping") to the QEMU user-mode gateway and, when the host allows it, to
internet addresses. Then UDP, DNS and DHCP.

## Target hardware and test setup

- **NIC:** Intel 82540EM (PCI `8086:100E`, the `e1000` device QEMU emulates).
  Chosen because QEMU emulates it well, the register interface is documented,
  and it is common on real hardware. Real-hardware support is not claimed.
- **QEMU:** `-netdev user,id=n0 -device e1000,netdev=n0`. QEMU's user network
  gives the guest `10.0.2.15/24`, gateway `10.0.2.2`, DNS `10.0.2.3`, and
  routes outbound traffic through the host.
- **Addressing:** static for now (`10.0.2.15/24`, gateway `10.0.2.2`). DHCP is
  stage 7.

## Design

Polling, not interrupts. The keyboard and mouse already poll, and polling keeps
the first version small. The receive ring is checked from `net_poll()`, which
the ping wait loop calls.

Layers, bottom up (one file each, all prefixed `net_` or `e1000`):

| File | Role |
| --- | --- |
| `e1000.c/.h` | PCI discovery, MMIO registers, descriptor rings, MAC address, send and receive of raw Ethernet frames |
| `net.c/.h` | Interface state (MAC, IP, gateway, netmask), `net_poll()`, dispatch by EtherType |
| `net_eth.c/.h` | Ethernet header build and parse |
| `net_arp.c/.h` | ARP request and reply, small cache, resolution of the next hop |
| `net_ip.c/.h` | IPv4 header build and parse, header checksum, protocol dispatch, subnet routing |
| `net_icmp.c/.h` | ICMP echo request and reply, reply matching by id and sequence |

Packet buffers are 2 KiB DMA buffers from the physical frame allocator. The
device needs physical addresses, and the kernel reaches the same memory through
the HHDM (higher-half direct map) offset, which `pmm` will expose.

MMIO registers are mapped with `vmm_map` and the cache-disable bit, the same way
the USB host controller maps its registers. QEMU does not check the caching
attribute, but real hardware does, so the cache-disable bit is set deliberately.

## Stages

1. **e1000 driver — built and QEMU-tested.** Code: `kernel/src/e1000.c`. Finds
   the card, enables bus mastering and memory decoding, maps BAR0 uncached,
   resets, reads the MAC, sets up the receive ring (32 descriptors) and transmit
   ring (8 descriptors), and sends/receives raw frames. Test: `net` prints the MAC,
   link state and frame counters; `net test` sends a broadcast ARP request for the
   gateway and waits for QEMU's reply. (The plan first said "a frame sent to our own
   MAC comes back". QEMU's user-mode network does not loop frames back, so the test
   uses a real reply from the gateway instead. It proves transmit, receive and DMA
   end to end.)
2. **Ethernet and ARP — built and QEMU-tested.** Code: `net.c`, `net_ethernet.c`, `net_arp.c`.
   The frame builder fills the Ethernet header. ARP answers requests for our address and
   learns every sender into a 16-entry round-robin cache. `net_arp_resolve` broadcasts a
   request, re-asks every 250 ms, and polls until a reply arrives or the timeout passes.
   Test: `net test` (stage 1) gets the gateway's reply, and the reply is learned into the
   cache; the ping in stage 3 resolves the gateway through the cache.
3. **IPv4 and ICMP echo — built and QEMU-tested.** Code: `net_ip.c`, `net_icmp.c`.
   IPv4 builds a 20-byte header (Don't Fragment, TTL 64), checks the checksum on receive,
   drops fragments, and routes to the next hop: the destination on our subnet, else the
   gateway. ICMP answers echo requests with a copy of the payload, and `net_ping` matches
   replies on identifier, sequence and sender. Test: `ping 10.0.2.2` gets replies
   (`tests/shell-test.py`: `reply from 10.0.2.2: seq=1`). A dead address (`10.0.2.99`)
   times out. The round trip is in microseconds, measured with the TSC (calibrated at boot,
   so it is only as accurate as that calibration; under TCG it is an emulated clock).
4. **Ring-3 `ping` — built and QEMU-tested.** `SYS_NET_PING` (syscall 36, `syscall_net.c`) takes
   the IPv4 address as one big-endian value, the sequence number and a timeout (1–10000 ms),
   and returns the round-trip time in microseconds, or -1 on timeout. The round trip is
   timed with the TSC after the next hop is resolved (`net_ip_warm`), so the first ARP
   wait is not counted. `/bin/ping` (`userspace/bin/ping.c`) prints each reply and a
   summary. Test: `tests/shell-test.py` runs `ping 10.0.2.2` from the ring-3 shell and
   checks the reply and the `4 sent, 4 received` summary.
5. **Internet addresses.** Ping `8.8.8.8` through the gateway. This depends on the
   host: QEMU's user network answers ICMP only if the host permits unprivileged
   ICMP. **Checked here: the host has `ping_group_range` 1 0, so unprivileged ICMP is
   disabled and `ping 8.8.8.8` times out.** That is the host's policy, not a stack
   result; a host that allows unprivileged ICMP would need a fresh run to confirm.
6. **UDP and DNS — built and QEMU-tested.** Code: `net_udp.c`, `net_dns.c`.
   UDP has four bound ports, each with a four-datagram queue, and checks the length field;
   checksums are 0 (legal for UDP over IPv4). The DNS client sends one A query to the
   configured server (10.0.2.3 under QEMU) from port 40053, three attempts, and parses the
   answer with name compression. Kernel command `nslookup <name>`; ring-3 `/bin/nslookup`
   through `SYS_NET_RESOLVE` (syscall 37). Tests: `nslookup nonexistent.invalid` gets
   "not found" from the kernel shell and from ring 3. Also checked by hand: `github.com` and
   `pypi.org` resolve to the same addresses as the host's own resolver, over UDP through
   QEMU's DNS. `ping <name>` in the kernel shell resolves the name first.
7. **DHCP — built and QEMU-tested.** Code: `net_dhcp.c`. Discover (broadcast from
   0.0.0.0), offer, request, ack; the ack sets the address, mask, router, DNS server and
   lease time. Kernel command `dhcp`. Test: `dhcp` gets `10.0.2.15` from QEMU's server.
   Not done: it runs when asked, not at boot; no renewal or rebinding (the lease time is
   reported, not counted down); no ring-3 `dhcp`.
8. **TCP — built and QEMU-tested (client only).** Code: `net_tcp.c`, `net_tcp.h`.
   Active opens only: `net_tcp_connect` sends SYN with a 1400-byte MSS and waits for SYN-ACK
   (retrying the SYN once a second). Four connection slots. Each slot has an 8 KB receive
   ring, and the window it advertises is the free space in that ring. Sends go out in
   segments of up to 1400 bytes, limited by the peer's window, and wait for ACKs; on a
   timeout they go back to the first unacknowledged byte and resend (go-back-N, five
   retries, one-second timeout). Out-of-order segments are dropped and re-acknowledged.
   Close sends FIN and waits up to a second for the peer's FIN. The checksum covers the
   pseudo-header and is summed in place, so a segment of any length up to the frame
   size is checked. Kernel command `tcpget <a.b.c.d> <port> <path>` sends a
   HTTP/1.0 GET and prints the byte count and the status line.
   Tests: `tcpget` to a closed port reports the connect failure (in the suite). Checked by
   hand: `tcpget 10.0.2.2 8765 /` against a local `python3 -m http.server` on the host
   (QEMU maps 10.0.2.2 to the host's loopback) receives the whole 16512-byte response.
   Not done: no listening sockets, no window scaling, SACK, timestamps or fast
   retransmit (a lost segment waits for the timeout), no congestion control, no
   zero-window persist timer of its own, and no TIME_WAIT. Only one open at a time is
   tested.

## Known limits (stages 1–8)

- One interface, one static address, no routing table beyond "the subnet, else
  the gateway".
- No IP fragmentation. Packets larger than the MTU are dropped with a log line.
- No retransmission. A ping that gets no reply times out. DNS retries its query twice.
- UDP checksums are not computed or checked. A datagram to a port nobody bound is dropped
  silently (no ICMP port unreachable).
- DHCP has no renewal, so the lease expires silently after its time.
- No DNS cache. No ARP aging.
- Polling only, so a ping spins the CPU while it waits. Acceptable until the
  scheduler can block on the device.
- Not tested on real hardware.

## Tests

`tests/net-test.py` (added in stage 4) boots with the `-netdev`/`-device` pair
above and checks the stage's results. It is run by `make test` once it is
stable. Each stage lists what its test checks.
