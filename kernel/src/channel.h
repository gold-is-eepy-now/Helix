/* channel.h — bounded, non-blocking kernel message channels. */
#ifndef HELIX_CHANNEL_H
#define HELIX_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "object.h"

#define CHANNEL_MAX_CAPACITY 8u
#define CHANNEL_MAX_MESSAGE_SIZE 128u

typedef enum {
    CHANNEL_OK = 0,
    CHANNEL_EMPTY,
    CHANNEL_FULL,
    CHANNEL_TOO_LARGE,
    CHANNEL_BUFFER_SMALL,
    CHANNEL_CLOSED,
    CHANNEL_DENIED,
    CHANNEL_WOULD_BLOCK,
    CHANNEL_CAPABILITY_PRESENT,
    CHANNEL_NO_CAPABILITY,
    CHANNEL_HANDLE_TABLE_FULL,
    CHANNEL_TIMED_OUT,
} channel_result_t;

/* Create a bounded FIFO pair with distinct send-only and receive-only handles.
 * Duplicating either handle may reduce its rights through the handle manager.
 */
bool channel_create_pair(handle_table_t *table, uint32_t capacity,
                         handle_t *send_handle, handle_t *receive_handle);
bool channel_create_pair_between(handle_table_t *send_table,
                                 handle_table_t *receive_table,
                                 uint32_t capacity,
                                 handle_t *send_handle,
                                 handle_t *receive_handle);
channel_result_t channel_send(handle_table_t *table, handle_t channel,
                              const void *message, size_t size);
/* Send a derived capability. requested_rights must be a subset of the source
 * capability; the receiver gets a fresh handle in its own handle table.
 */
channel_result_t channel_send_capability(handle_table_t *table,
                                         handle_t channel,
                                         handle_t capability,
                                         uint32_t requested_rights);
channel_result_t channel_receive(handle_table_t *table, handle_t channel,
                                 void *buffer, size_t buffer_size,
                                 size_t *message_size);
/* Wait for a message when the queue is empty. Returns CHANNEL_WOULD_BLOCK if
 * no other kernel thread is runnable to wake this caller.
 */
channel_result_t channel_receive_wait(handle_table_t *table, handle_t channel,
                                     void *buffer, size_t buffer_size,
                                     size_t *message_size);
/* Timeout is expressed in PIT ticks (100 ticks per second). */
channel_result_t channel_receive_timeout(handle_table_t *table,
                                         handle_t channel,
                                         void *buffer, size_t buffer_size,
                                         size_t *message_size,
                                         uint64_t timeout_ticks);
channel_result_t channel_receive_capability(handle_table_t *table,
                                            handle_t channel,
                                            handle_t *capability);

#endif /* HELIX_CHANNEL_H */
