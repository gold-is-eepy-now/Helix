/* mach_port.h — Mach ports, tasks, and IPC message definitions.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#ifndef MAC_DARWIN_MACH_PORT_H
#define MAC_DARWIN_MACH_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t mach_port_t;
typedef uint32_t mach_port_right_t;
typedef int32_t  kern_return_t;
typedef uint32_t mach_msg_bits_t;
typedef uint32_t mach_msg_size_t;
typedef int32_t  mach_msg_id_t;
typedef uint32_t mach_msg_timeout_t;

#define MACH_PORT_NULL              ((mach_port_t)0)
#define MACH_PORT_DEAD              ((mach_port_t)~0)

/* Mach Port Rights */
#define MACH_PORT_RIGHT_SEND        ((mach_port_right_t)0)
#define MACH_PORT_RIGHT_RECEIVE     ((mach_port_right_t)1)
#define MACH_PORT_RIGHT_SEND_ONCE   ((mach_port_right_t)2)
#define MACH_PORT_RIGHT_PORT_SET    ((mach_port_right_t)3)
#define MACH_PORT_RIGHT_DEAD_NAME   ((mach_port_right_t)4)

/* Mach Error Codes */
#define KERN_SUCCESS                0
#define KERN_INVALID_ADDRESS        1
#define KERN_PROTECTION_FAILURE     2
#define KERN_NO_SPACE               3
#define KERN_INVALID_ARGUMENT       4
#define KERN_FAILURE                5
#define KERN_RESOURCE_SHORTAGE      6
#define KERN_NOT_RECEIVER           7
#define KERN_NO_ACCESS              8
#define KERN_MEMORY_FAILURE         9
#define KERN_MEMORY_ERROR           10
#define KERN_ALREADY_IN_SET         11
#define KERN_NOT_IN_SET             12
#define KERN_NAME_EXISTS            13
#define KERN_ABORTED                14
#define KERN_INVALID_NAME           15
#define KERN_INVALID_TASK           16
#define KERN_INVALID_RIGHT          17
#define KERN_INVALID_VALUE          18
#define KERN_UREFS_OVERFLOW         19
#define KERN_INVALID_CAPABILITY     20
#define KERN_RIGHT_EXISTS           21
#define KERN_INVALID_HOST           22
#define KERN_MEMORY_PRESENT         23

/* Mach Message Options */
#define MACH_MSG_OPTION_NONE        0x00000000
#define MACH_SEND_MSG               0x00000001
#define MACH_RCV_MSG                0x00000002
#define MACH_RCV_TIMEOUT            0x00000100
#define MACH_RCV_INTERRUPT          0x00000400

#define MACH_MSG_SUCCESS            0
#define MACH_SEND_INVALID_DATA      0x10000002
#define MACH_SEND_INVALID_DEST      0x10000003
#define MACH_SEND_TIMED_OUT         0x10000004
#define MACH_RCV_INVALID_NAME       0x10004001
#define MACH_RCV_TIMED_OUT          0x10004003
#define MACH_RCV_TOO_LARGE          0x10004004

/* Mach Message Header */
typedef struct {
    mach_msg_bits_t msgh_bits;
    mach_msg_size_t msgh_size;
    mach_port_t     msgh_remote_port;
    mach_port_t     msgh_local_port;
    mach_msg_size_t msgh_reserved;
    mach_msg_id_t   msgh_id;
} mach_msg_header_t;

/* Port Management API */
void        mach_port_subsystem_init(void);
mach_port_t mach_task_self(void);
mach_port_t mach_host_self(void);
mach_port_t mach_reply_port(void);
kern_return_t mach_port_allocate(mach_port_t task, mach_port_right_t right, mach_port_t *name);
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name);
kern_return_t mach_port_insert_right(mach_port_t task, mach_port_t name, mach_port_t poly, mach_msg_bits_t polyPoly);

/* Mach Message Trap Implementation */
kern_return_t mach_msg_trap_impl(mach_msg_header_t *msg, uint32_t option,
                                 mach_msg_size_t send_size, mach_msg_size_t rcv_size,
                                 mach_port_t rcv_name, mach_msg_timeout_t timeout,
                                 mach_port_t notify);

#ifdef __cplusplus
}
#endif

#endif /* MAC_DARWIN_MACH_PORT_H */
