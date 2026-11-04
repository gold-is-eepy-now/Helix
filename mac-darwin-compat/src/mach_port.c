/* mach_port.c — Mach ports and IPC messaging implementation.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include "../include/mach_port.h"
#include <string.h>

#define MAX_MACH_PORTS 128
#define MAX_QUEUED_MESSAGES 32
#define MAX_MSG_DATA 1024

typedef struct {
    mach_msg_header_t header;
    uint8_t           body[MAX_MSG_DATA];
    size_t            body_size;
    bool              in_use;
} queued_mach_msg_t;

typedef struct {
    mach_port_t        name;
    mach_port_right_t  rights;
    uint32_t           ref_count;
    bool               in_use;
    queued_mach_msg_t  queue[MAX_QUEUED_MESSAGES];
    uint32_t           msg_head;
    uint32_t           msg_tail;
    uint32_t           msg_count;
} mach_port_entry_t;

static mach_port_entry_t g_port_table[MAX_MACH_PORTS];
static mach_port_t g_next_port_name = 100;
static bool g_initialized = false;

#define SPECIAL_PORT_TASK_SELF 1
#define SPECIAL_PORT_HOST_SELF 2

void mach_port_subsystem_init(void) {
    if (g_initialized) return;
    memset(g_port_table, 0, sizeof(g_port_table));

    /* Slot 0 is reserved / MACH_PORT_NULL */

    /* Special Port: Task Self */
    g_port_table[1].name = SPECIAL_PORT_TASK_SELF;
    g_port_table[1].rights = MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND;
    g_port_table[1].ref_count = 1;
    g_port_table[1].in_use = true;

    /* Special Port: Host Self */
    g_port_table[2].name = SPECIAL_PORT_HOST_SELF;
    g_port_table[2].rights = MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND;
    g_port_table[2].ref_count = 1;
    g_port_table[2].in_use = true;

    g_initialized = true;
}

static mach_port_entry_t *find_port(mach_port_t name) {
    if (!g_initialized) mach_port_subsystem_init();
    if (name == MACH_PORT_NULL || name == MACH_PORT_DEAD) return NULL;

    for (int i = 1; i < MAX_MACH_PORTS; i++) {
        if (g_port_table[i].in_use && g_port_table[i].name == name) {
            return &g_port_table[i];
        }
    }
    return NULL;
}

mach_port_t mach_task_self(void) {
    if (!g_initialized) mach_port_subsystem_init();
    return SPECIAL_PORT_TASK_SELF;
}

mach_port_t mach_host_self(void) {
    if (!g_initialized) mach_port_subsystem_init();
    return SPECIAL_PORT_HOST_SELF;
}

mach_port_t mach_reply_port(void) {
    mach_port_t port = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS) {
        return port;
    }
    return MACH_PORT_NULL;
}

kern_return_t mach_port_allocate(mach_port_t task, mach_port_right_t right, mach_port_t *name) {
    (void)task;
    if (!g_initialized) mach_port_subsystem_init();
    if (!name) return KERN_INVALID_ARGUMENT;

    for (int i = 3; i < MAX_MACH_PORTS; i++) {
        if (!g_port_table[i].in_use) {
            mach_port_entry_t *p = &g_port_table[i];
            memset(p, 0, sizeof(*p));
            p->name = g_next_port_name++;
            p->rights = right;
            p->ref_count = 1;
            p->in_use = true;
            *name = p->name;
            return KERN_SUCCESS;
        }
    }

    return KERN_RESOURCE_SHORTAGE;
}

kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name) {
    (void)task;
    mach_port_entry_t *p = find_port(name);
    if (!p) return KERN_INVALID_NAME;

    if (p->ref_count > 0) {
        p->ref_count--;
    }
    if (p->ref_count == 0) {
        p->in_use = false;
        p->msg_count = 0;
    }

    return KERN_SUCCESS;
}

kern_return_t mach_port_insert_right(mach_port_t task, mach_port_t name, mach_port_t poly, mach_msg_bits_t polyPoly) {
    (void)task; (void)poly; (void)polyPoly;
    mach_port_entry_t *p = find_port(name);
    if (!p) return KERN_INVALID_NAME;
    p->ref_count++;
    return KERN_SUCCESS;
}

kern_return_t mach_msg_trap_impl(mach_msg_header_t *msg, uint32_t option,
                                 mach_msg_size_t send_size, mach_msg_size_t rcv_size,
                                 mach_port_t rcv_name, mach_msg_timeout_t timeout,
                                 mach_port_t notify) {
    (void)timeout; (void)notify;
    if (!g_initialized) mach_port_subsystem_init();
    if (!msg) return KERN_INVALID_ARGUMENT;

    /* Handle send */
    if (option & MACH_SEND_MSG) {
        mach_port_entry_t *dest = find_port(msg->msgh_remote_port);
        if (!dest) return MACH_SEND_INVALID_DEST;

        if (dest->msg_count < MAX_QUEUED_MESSAGES) {
            queued_mach_msg_t *slot = &dest->queue[dest->msg_tail];
            dest->msg_tail = (dest->msg_tail + 1) % MAX_QUEUED_MESSAGES;
            dest->msg_count++;

            memcpy(&slot->header, msg, sizeof(mach_msg_header_t));
            size_t body_len = 0;
            if (send_size > sizeof(mach_msg_header_t)) {
                body_len = send_size - sizeof(mach_msg_header_t);
                if (body_len > MAX_MSG_DATA) body_len = MAX_MSG_DATA;
                memcpy(slot->body, ((const uint8_t *)msg) + sizeof(mach_msg_header_t), body_len);
            }
            slot->body_size = body_len;
            slot->in_use = true;
        } else {
            return MACH_SEND_TIMED_OUT;
        }
    }

    /* Handle receive */
    if (option & MACH_RCV_MSG) {
        mach_port_entry_t *src = find_port(rcv_name);
        if (!src) return MACH_RCV_INVALID_NAME;

        if (src->msg_count > 0) {
            queued_mach_msg_t *slot = &src->queue[src->msg_head];
            src->msg_head = (src->msg_head + 1) % MAX_QUEUED_MESSAGES;
            src->msg_count--;

            size_t needed = sizeof(mach_msg_header_t) + slot->body_size;
            if (rcv_size < needed) {
                return MACH_RCV_TOO_LARGE;
            }

            memcpy(msg, &slot->header, sizeof(mach_msg_header_t));
            if (slot->body_size > 0) {
                memcpy(((uint8_t *)msg) + sizeof(mach_msg_header_t), slot->body, slot->body_size);
            }
            slot->in_use = false;
            return MACH_MSG_SUCCESS;
        } else {
            /* Queue is empty */
            return MACH_RCV_TIMED_OUT;
        }
    }

    return MACH_MSG_SUCCESS;
}
