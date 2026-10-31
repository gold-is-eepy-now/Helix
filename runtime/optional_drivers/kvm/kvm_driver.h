/* kvm_driver.h — KVM virtualization host driver.
 *
 * KVM host driver for Helix OS. Provides the /dev/kvm device interface
 * for managing KVM virtual machines. This is a skeleton implementation
 * intended for prototyping KVM host support in Helix.
 *
 * The driver implements a subset of Linux KVM ioctl interface. Full
 * KVM host support requires VMX/VT-x extensions, EPT support, VMEXIT
 * handling, and VM management.
 */

#ifndef HELIX_KVM_DRIVER_H
#define HELIX_KVM_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* KVM ioctl command definitions (subset of Linux KVM API) */
#define KVM_GET_API_VERSION 0x0000
#define KVM_CREATE_VM       0x0001
#define KVM_CREATE_VCPU     0x0041
#define KVM_SET_USER_MEMORY_REGION 0x0040
#define KVM_RUN             0x0080
#define KVM_GET_MSR_INDEX   0x0082
#define KVM_SET_MSR_INDEX   0x0083
#define KVM_CAP_VENDOR_HINTS 0x1000

/* KVM VCPU run states */
#define KVM_EXIT_UNKNOWN    0x00000000
#define KVM_EXIT_SPLIT_LOCK 0x00000001
#define KVM_EXIT_IO         0x00000002
#define KVM_EXIT_HYpercALL  0x00000003
#define KVM_EXIT_EPR        0x00000005
#define KVM_EXIT_SYSTEM_EVENT 0x00000005
#define KVM_EXIT_SHUTDOWN   0x00000006
#define KVM_EXIT_FAIL_ENTRY 0x00000007
#define KVM_EXIT_INTR       0x00000008

/* KVM capabilities */
#define KVM_CAP_USER_MEMORY 0x1
#define KVM_CAP_VCPU_EVENTS 0x2
#define KVM_CAP_CLOCK_SOURCES 0x3

/* KVM VM context */
typedef struct {
    int vm_fd;                 /* KVM VM file descriptor */
    int vcpu_fd;               /* Current VCPU file descriptor */
    uint64_t memory_size;
    uint64_t memory_base;
    uint32_t vcpu_count;
    uint32_t max_vcpus;
    bool active;
} kvm_vm_context_t;

/* KVM VCPU context */
typedef struct {
    int vcpu_fd;               /* VCPU file descriptor */
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cr0, cr3, cr4;
    uint64_t efer;
    uint64_t guest_phys_base;
    uint64_t memory_size;
    bool in_run;
} kvm_vcpu_context_t;

/* Global KVM host state */
typedef struct {
    int dev_kvm_fd;            /* /dev/kvm file descriptor */
    uint32_t api_version;
    kvm_vm_context_t *vms;
    uint32_t vm_count;
    uint32_t max_vms;
    bool initialized;
} kvm_host_state_t;

extern kvm_host_state_t g_kvm_host_state;

void register_kvm_driver(void);
void deregister_kvm_driver(void);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_KVM_DRIVER_H */