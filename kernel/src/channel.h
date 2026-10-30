/* channel.h — rendezvous channels for kernel threads.
 *
 * A channel carries a single message from a sender to one receiver. Either
 * side may park: the sender parks while no receiver is waiting; the receiver
 * parks while no sender has a value pending. The first side to arrive
 * consumes the rendezvous and hands the CPU back to the other side.
 */
#ifndef HELIX_CHANNEL_H
#define HELIX_CHANNEL_H

#include <stdbool.h>
#include <stdint.h>

#include "thread.h"

typedef struct channel {
    uint64_t value;

    struct thread *sender_waiting;
    thread_context_t sender_ctx;
    uint64_t sender_value;

    struct thread *receiver_waiting;
    thread_context_t receiver_ctx;
} channel_t;

channel_t *channel_default(void);
void channel_init(void);
void channel_send(channel_t *channel, uint64_t value);
bool channel_recv(channel_t *channel, uint64_t *out);

#endif /* HELIX_CHANNEL_H */
