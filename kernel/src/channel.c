/* channel.c — rendezvous channels for kernel threads.
 *
 * The channel holds at most one outstanding message. `channel_send` either
 * delivers to a parked receiver or parks the sender (saving its full context
 * via thread_save_current so it can be resumed later). `channel_recv` either
 * takes the parked sender's value or parks the receiver. The delivery paths
 * hand the CPU over with thread_resume_context, which never returns for the
 * delivering side; the consuming side simply returns to its caller.
 */
#include "channel.h"

#include <stddef.h>
#include <stdint.h>

#include "thread.h"

/* The default rendezvous channel backs onto plain kernel storage: it belongs
 * to the scheduler demo rather than the object manager, so it never shows up
 * in the object registry's live-object count. Object-backed channels are
 * created through the object manager by whoever needs handle semantics. */
static channel_t g_default_channel;

static channel_t *g_channel;

channel_t *channel_default(void) { return g_channel; }

void channel_init(void) {
    g_channel = &g_default_channel;
}

void channel_send(channel_t *channel, uint64_t value) {
    if (channel->receiver_waiting != NULL) {
        /* A receiver is parked: deliver and let it run. */
        struct thread *receiver = channel->receiver_waiting;
        channel->receiver_waiting = NULL;
        channel->sender_value = value;
        thread_set_state(receiver, THREAD_READY);
        thread_set_state(thread_current(), THREAD_READY);
        thread_resume_context(&channel->receiver_ctx, receiver);
        /* thread_resume_context never returns; the receiver owns the CPU. */
    }

    struct thread *sender = thread_current();
    channel->sender_waiting = sender;
    channel->sender_value = value;
    thread_set_state(sender, THREAD_WAITING);
    thread_save_current(&channel->sender_ctx);
    thread_block_current();
}

bool channel_recv(channel_t *channel, uint64_t *out) {
    if (channel->sender_waiting != NULL) {
        /* A sender is parked with a value: take it. */
        struct thread *sender = channel->sender_waiting;
        channel->sender_waiting = NULL;
        *out = channel->sender_value;
        thread_set_state(sender, THREAD_READY);
        return true;
    }

    struct thread *receiver = thread_current();
    channel->receiver_waiting = receiver;
    thread_set_state(receiver, THREAD_WAITING);
    thread_save_current(&channel->receiver_ctx);
    thread_block_current();
    /* Fall-through is only reachable when a delivery resumed us: the deliver
     * path marks the receiver READY, so thread_block_current fell through
     * instead of parking. Consume the delivered value and return to caller. */
    *out = channel->sender_value;
    thread_set_state(receiver, THREAD_RUNNING);
    return true;
}
