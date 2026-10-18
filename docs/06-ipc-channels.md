# M8: IPC channels

The first channel slice adds bounded, non-blocking kernel message queues. A
channel pair has separate send-only and receive-only handles, backed by one
shared FIFO. Handle rights are checked on every operation. Closing the final
handle for an endpoint makes the peer observe `CHANNEL_CLOSED` once its queued
messages have been drained.

The initial limits are eight queued messages per channel and 128 bytes per
message. A full queue returns `CHANNEL_FULL`; an empty queue returns
`CHANNEL_EMPTY` while a sender remains open. If the receive buffer is too
small, the message stays queued and the required size is returned.

The API is in `kernel/src/channel.h`. `channel_create_pair()` creates one
send handle and one receive handle in a handle table. `channel_send()` and
`channel_receive()` are non-blocking. `channel_receive_wait()` sleeps the
calling kernel thread while the queue is empty and returns `CHANNEL_WOULD_BLOCK`
if no other thread is runnable. A send wakes one waiter; closing the final
sender or receiver wakes all waiters so they can observe closure.

Operations are protected against PIT preemption on the current single CPU.
`chantest` covers queue limits, endpoint rights, FIFO reads, closure and
object cleanup; it has passed in QEMU. `chanwaittest` covers blocking receive,
message wake-up, close wake-up and the no-runnable-thread case; it still needs
machine testing. Capability transfer, timeouts and cross-process IPC remain
future work. These queues are kernel objects and do not yet provide user-space
IPC.

Blocking receive, wakeups, timeouts, and cross-process handle transfer remain
future M8 work. These queues are kernel objects; they do not yet provide user
space IPC.
