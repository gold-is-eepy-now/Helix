/* channel.c — bounded kernel message channels with optional blocking receive. */
#include "channel.h"

#include "heap.h"
#include "kstring.h"
#include "object.h"
#include "sync.h"
#include "thread.h"

typedef struct {
    size_t size;
    uint8_t data[CHANNEL_MAX_MESSAGE_SIZE];
} channel_message_t;

typedef struct {
    uint32_t capacity;
    uint32_t head;
    uint32_t count;
    uint32_t endpoints;
    uint32_t senders;
    uint32_t receivers;
    uint32_t waiter_count;
    thread_id_t waiters[KERNEL_THREAD_MAX];
    channel_message_t messages[];
} channel_core_t;

typedef struct {
    channel_core_t *core;
    bool can_send;
    bool can_receive;
} channel_endpoint_t;

static void wake_waiters(channel_core_t *core, bool all) {
    while (core->waiter_count != 0) {
        thread_id_t id = core->waiters[0];
        for (uint32_t i = 1; i < core->waiter_count; i++)
            core->waiters[i - 1] = core->waiters[i];
        core->waiter_count--;
        if (thread_wake(id) && !all) break;
    }
}

static bool add_waiter(channel_core_t *core, thread_id_t id) {
    if (id == 0 || core->waiter_count == KERNEL_THREAD_MAX) return false;
    for (uint32_t i = 0; i < core->waiter_count; i++)
        if (core->waiters[i] == id) return true;
    core->waiters[core->waiter_count++] = id;
    return true;
}

static void remove_waiter(channel_core_t *core, thread_id_t id) {
    for (uint32_t i = 0; i < core->waiter_count; i++) {
        if (core->waiters[i] != id) continue;
        for (uint32_t j = i + 1; j < core->waiter_count; j++)
            core->waiters[j - 1] = core->waiters[j];
        core->waiter_count--;
        return;
    }
}

static void channel_endpoint_destroy(object_t *object, void *payload_ptr) {
    (void)object;
    channel_endpoint_t *endpoint = payload_ptr;
    if (endpoint == NULL || endpoint->core == NULL) return;
    channel_core_t *core = endpoint->core;
    if (endpoint->can_send && core->senders != 0 && --core->senders == 0)
        wake_waiters(core, true);
    if (endpoint->can_receive && core->receivers != 0 && --core->receivers == 0)
        wake_waiters(core, true);
    if (core->endpoints != 0 && --core->endpoints == 0) kfree(core);
}

bool channel_create_pair(handle_table_t *table, uint32_t capacity,
                         handle_t *send_handle, handle_t *receive_handle) {
    IRQ_GUARD;
    if (table == NULL || send_handle == NULL || receive_handle == NULL ||
        send_handle == receive_handle ||
        capacity == 0 || capacity > CHANNEL_MAX_CAPACITY) return false;
    *send_handle = 0;
    *receive_handle = 0;

    size_t payload_size = sizeof(channel_core_t) +
        (size_t)capacity * sizeof(channel_message_t);
    channel_core_t *core = kzalloc(payload_size);
    if (core == NULL) return false;
    core->capacity = capacity;

    object_t *sender = object_create(OBJECT_TYPE_CHANNEL,
        sizeof(channel_endpoint_t), channel_endpoint_destroy);
    if (sender == NULL) {
        kfree(core);
        return false;
    }
    channel_endpoint_t *send_endpoint = object_payload(sender);
    send_endpoint->core = core;
    send_endpoint->can_send = true;
    core->endpoints++;
    core->senders++;

    object_t *receiver = object_create(OBJECT_TYPE_CHANNEL,
        sizeof(channel_endpoint_t), channel_endpoint_destroy);
    if (receiver == NULL) {
        object_release(sender);
        return false;
    }
    channel_endpoint_t *receive_endpoint = object_payload(receiver);
    receive_endpoint->core = core;
    receive_endpoint->can_receive = true;
    core->endpoints++;
    core->receivers++;

    handle_t send = handle_create(table, sender,
        OBJECT_RIGHT_WRITE | OBJECT_RIGHT_DUPLICATE | OBJECT_RIGHT_INSPECT);
    handle_t receive = handle_create(table, receiver,
        OBJECT_RIGHT_READ | OBJECT_RIGHT_DUPLICATE | OBJECT_RIGHT_INSPECT);
    if (send == 0 || receive == 0) {
        if (send != 0) handle_close(table, send);
        if (receive != 0) handle_close(table, receive);
        object_release(sender);
        object_release(receiver);
        return false;
    }
    object_release(sender);
    object_release(receiver);
    *send_handle = send;
    *receive_handle = receive;
    return true;
}

channel_result_t channel_send(handle_table_t *table, handle_t channel,
                              const void *message, size_t size) {
    IRQ_GUARD;
    if (size > CHANNEL_MAX_MESSAGE_SIZE) return CHANNEL_TOO_LARGE;
    if (size != 0 && message == NULL) return CHANNEL_DENIED;

    object_t *object = handle_get(table, channel, OBJECT_RIGHT_WRITE,
                                  OBJECT_TYPE_CHANNEL);
    channel_endpoint_t *endpoint = object_payload(object);
    channel_core_t *core = endpoint != NULL && endpoint->can_send
        ? endpoint->core : NULL;
    if (core == NULL) {
        object_release(object);
        return CHANNEL_DENIED;
    }
    if (core->receivers == 0) {
        object_release(object);
        return CHANNEL_CLOSED;
    }
    if (core->count == core->capacity) {
        object_release(object);
        return CHANNEL_FULL;
    }

    uint32_t tail = (core->head + core->count) % core->capacity;
    channel_message_t *slot = &core->messages[tail];
    slot->size = size;
    if (size != 0) memcpy(slot->data, message, size);
    core->count++;
    wake_waiters(core, false);
    object_release(object);
    return CHANNEL_OK;
}

channel_result_t channel_receive(handle_table_t *table, handle_t channel,
                                 void *buffer, size_t buffer_size,
                                 size_t *message_size) {
    IRQ_GUARD;
    object_t *object = handle_get(table, channel, OBJECT_RIGHT_READ,
                                  OBJECT_TYPE_CHANNEL);
    channel_endpoint_t *endpoint = object_payload(object);
    channel_core_t *core = endpoint != NULL && endpoint->can_receive
        ? endpoint->core : NULL;
    if (core == NULL) {
        object_release(object);
        return CHANNEL_DENIED;
    }
    if (core->count == 0) {
        object_release(object);
        return core->senders == 0 ? CHANNEL_CLOSED : CHANNEL_EMPTY;
    }

    channel_message_t *slot = &core->messages[core->head];
    if (buffer_size < slot->size || (slot->size != 0 && buffer == NULL)) {
        if (message_size != NULL) *message_size = slot->size;
        object_release(object);
        return CHANNEL_BUFFER_SMALL;
    }

    if (slot->size != 0) memcpy(buffer, slot->data, slot->size);
    if (message_size != NULL) *message_size = slot->size;
    slot->size = 0;
    core->head = (core->head + 1) % core->capacity;
    core->count--;
    object_release(object);
    return CHANNEL_OK;
}

channel_result_t channel_receive_wait(handle_table_t *table, handle_t channel,
                                      void *buffer, size_t buffer_size,
                                      size_t *message_size) {
    for (;;) {
        channel_result_t result = channel_receive(table, channel, buffer,
                                                   buffer_size, message_size);
        if (result != CHANNEL_EMPTY) return result;

        IRQ_GUARD;
        object_t *object = handle_get(table, channel, OBJECT_RIGHT_READ,
                                      OBJECT_TYPE_CHANNEL);
        channel_endpoint_t *endpoint = object_payload(object);
        channel_core_t *core = endpoint != NULL && endpoint->can_receive
            ? endpoint->core : NULL;
        if (core == NULL) {
            object_release(object);
            return CHANNEL_DENIED;
        }
        /* Recheck with interrupts disabled to close the empty-to-sleep race. */
        if (core->count != 0 || core->senders == 0) {
            object_release(object);
            continue;
        }

        thread_id_t id = thread_current_id();
        if (!add_waiter(core, id)) {
            object_release(object);
            return CHANNEL_WOULD_BLOCK;
        }
        object_release(object);
        if (!thread_block_current()) {
            remove_waiter(core, id);
            return CHANNEL_WOULD_BLOCK;
        }
        /* A send or endpoint close woke us. Retry the queue and closure checks. */
    }
}
