# 10 — Network stack

Status: **in progress** (M12). Stage 1 is built and tested. Stages below are marked as they are built and
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
2. **Ethernet and ARP.** Resolve `10.0.2.2` to a MAC by ARP. Test: the ARP
   reply from the gateway is in the cache.
3. **IPv4 and ICMP echo.** Build and checksum echo requests, match replies.
   Test: `ping 10.0.2.2` gets replies.
4. **Ring-3 `ping`.** `SYS_NET_PING(ip, seq, timeout_ms)` returns the round-trip
   time in microseconds, or -1 on timeout. A ring-3 `/bin/ping` prints each
   reply and a summary. Test: `ping 10.0.2.2` from the ring-3 shell.
5. **Internet addresses.** Ping `8.8.8.8` through the gateway. This depends on the
   host: QEMU's user network answers ICMP only if the host permits unprivileged
   ICMP. If it does not, the test reports timeouts, not success.
6. **UDP and DNS.** UDP send/receive, then name resolution through `10.0.2.3`.
7. **DHCP.** Replace the static address with a lease.
8. **TCP.** Out of scope for this plan; separate document when started.

## Known limits (stages 1–5)

- One interface, one static address, no routing table beyond "the subnet, else
  the gateway".
- No IP fragmentation. Packets larger than the MTU are dropped with a log line.
- No retransmission. A ping that gets no reply times out.
- Polling only, so a ping spins the CPU while it waits. Acceptable until the
  scheduler can block on the device.
- Not tested on real hardware.

## Tests

`tests/net-test.py` (added in stage 4) boots with the `-netdev`/`-device` pair
above and checks the stage's results. It is run by `make test` once it is
stable. Each stage lists what its test checks.
