/* kvm_driver.h — KVM virtualization host driver.
 *
 * KVM host driver for Helix OS. Provides the /dev/kvm device interface
 * for managing KVM virtual machines.
 *
 * Implements the Linux KVM ioctl API:
 * - KVM_GET_API_VERSION
 * - KVM_CREATE_VM
 * - KVM_CREATE_VCPU
 * - KVM_SET_USER_MEMORY_REGION
 * - KVM_RUN
 *
 * This driver is intended for prototyping KVM host support in Helix.
 * Full KVM host support requires VMX/VT-x extensions and proper VMEXIT handling.
 */

#ifndef HELIX_KVM_DRIVER_H
#define HELIX_KVM_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* KVM ioctl commands */
#define KVM_GET_API_VERSION 0x0000
#define KVM_CREATE_VM       0x0001
#define KVM_CREATE_VCPU     0x0041
#define KVM_SET_USER_MEMORY_REGION 0x0040
#define KVM_RUN             0x0080

/* KVM userspace memory region structure */
struct kvm_userspace_memory_region {
    uint32_t slot;
    uint32_t flags;
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    uint64_t userspace_addr;
};

/* KVM run structure */
struct kvm_run {
    uint32_t exit_reason;
    uint32_t padding;
};

/* KVM handle type for the driver */
typedef struct {
    int fd;            /* /dev/kvm file descriptor */
    int vm_fd;         /* KVM VM file descriptor */
    int vcpu_fd;       /* VCPU file descriptor */
    uint32_t api_version;
    bool initialized;
} kvm_t;

/* Driver functions */
void register_kvm_driver(void);
void deregister_kvm_driver(void);

/* KVM ioctl wrappers */
void kvm_init(void);
int kvm_create_vm(kvm_t *kvm);
int kvm_create_vcpu(kvm_t *kvm, int *vcpu_fd);
int kvm_set_user_memory_region(kvm_t *kvm, uint64_t slot,
                               uint64_t guest_phys_addr,
                               uint64_t memory_size,
                               uint64_t userspace_addr);
int kvm_vcpu_run(int vcpu_fd);
void kvm_close(kvm_t *kvm);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_KVM_DRIVER_H */