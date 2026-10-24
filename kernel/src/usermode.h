/* usermode.h — controlled first ring-3 transition diagnostic. */
#ifndef HELIX_USERMODE_H
#define HELIX_USERMODE_H

#include <stdbool.h>
#include <stdint.h>

#include "idt.h"
#include "vmm.h"

bool usermode_test(uint64_t *result_out);
bool usermode_syscall_init(void);
bool usermode_run(vmm_address_space_t *space, uint64_t entry,
                 uint64_t user_rsp, uint64_t *result_out,
                 uint64_t *fault_vector_out);
void usermode_grant_next_uart_control(void);
void usermode_grant_next_usb_read(void);
bool usermode_fault_test(uint64_t *fault_vector_out);
bool usermode_driver_capability_test(void);
bool usermode_handle_exception(registers_t *frame);
bool usermode_handle_syscall(registers_t *frame);

#endif /* HELIX_USERMODE_H */
