/* kvm_driver.h — KVM virtualization driver header.
 *
 * Provides the interface for KVM-based virtualization support when Helix
 * runs as a guest under QEMU/KVM, or when Helix userspace programs want to
 * create VMs via /dev/kvm.
 *
 * This is NOT a hypervisor — Helix does not run other operating systems.
 * This driver provides guest-side KVM interfaces only.
 */

#ifndef HELIX_KVM_DRIVER_H
#define HELIX_KVM_DRIVER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* KVM ioctl command numbers (subset of Linux KVM API) */
#define KVM_GET_API_VERSION 0x0000
#define KVM_CREATE_VM       0x0001
#define KVM_CREATE_VCPU     0x0041
#define KVM_SET_USER_MEMORY_REGION 0x0040

/* KVM VCPU run exit reasons */
#define KVM_EXIT_IO        1
#define KVM_EXIT_MMIO      2
#define KVM_EXIT_HLT       3

/* KVM VCPU register identifiers */
#define KVM_REG_RAX 0
#define KVM_REG_RBX 1
#define KVM_REG_RCX 2
#define KVM_REG_RDX 3
#define KVM_REG_RSP 4
#define KVM_REG_RBP 5
#define KVM_REG_RSI 6
#define KVM_REG_RDI 7

/* -- KVM file descriptor handle ------------------------------------------- */

typedef struct {
    int fd;            /* /dev/kvm file descriptor */
    int vm_fd;         /* VM file descriptor */
    int vcpu_fd;       /* VCPU file descriptor */
    uint32_t api_version;
} kvm_t;

/* -- KVM driver functions -------------------------------------------------- */

/* Initialize KVM driver. Opens /dev/kvm and checks API version. */
int kvm_init(void);

/* Create a new KVM VM */
int kvm_create_vm(kvm_t *kvm);

/* Create a VCPU for a VM */
int kvm_create_vcpu(kvm_t *kvm, int *vcpu_fd);

/* Set a user memory region in a VM */
int kvm_set_user_memory_region(kvm_t *kvm, uint64_t slot,
                               uint64_t guest_phys_addr,
                               uint64_t memory_size,
                               uint64_t userspace_addr);

/* Run a VCPU */
int kvm_vcpu_run(int vcpu_fd);

/* Close KVM resources */
void kvm_close(kvm_t *kvm);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_KVM_DRIVER_H */