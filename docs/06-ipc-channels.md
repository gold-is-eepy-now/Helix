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
`channel_receive()` return `channel_result_t` values rather than blocking.
Operations are protected against PIT preemption on the current single CPU.
The kernel shell command `chantest` exercises endpoint rights, capacity,
message preservation on a small receive buffer, endpoint closure, and object
cleanup. Its result still needs machine testing before this slice is marked
complete.

Blocking receive, wakeups, timeouts, and cross-process handle transfer remain
future M8 work. These queues are kernel objects; they do not yet provide user
space IPC.
