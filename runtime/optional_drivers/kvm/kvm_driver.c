/* kvm_driver.c — KVM virtualization host driver.
 *
 * KVM host driver for Helix OS. Provides the /dev/kvm device interface
 * for managing KVM virtual machines. This is a skeleton implementation
 * intended for prototyping and testing KVM host functionality in Helix.
 *
 * The driver implements a subset of Linux KVM ioctl interface. Full
 * KVM host support requires VMX/VT-x extensions, EPT support, VMEXIT
 * handling, and VM management.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "kvm_driver.h"

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
#define KVM_EXIT_EPR        0x00000004
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

/* Global KVM state */
static struct {
    int dev_kvm_fd;            /* /dev/kvm file descriptor */
    uint32_t api_version;
    kvm_vm_context_t *vms;
    uint32_t vm_count;
    uint32_t max_vms;
    bool initialized;
} g_kvm_host_state;

/* Forward declarations */
static int kvm_host_init(void);
static void kvm_host_cleanup(void);
static int kvm_vm_create(kvm_vm_context_t *vm);
static int kvm_vm_destroy(kvm_vm_context_t *vm);
static int kvm_vcpu_create(kvm_vm_context_t *vm, kvm_vcpu_context_t *vcpu);
static int kvm_vcpu_destroy(kvm_vcpu_context_t *vcpu);
static int kvm_memory_map(kvm_vm_context_t *vm, uint64_t slot,
                         uint64_t gpa, uint64_t size, uint64_t hpa);
static int kvm_vcpu_run(kvm_vcpu_context_t *vcpu);
static int kvm_get_capabilities(void);

/* Driver registration */
static driver_t g_kvm_driver;

/* Driver initialization function */
static void kvm_driver_init(void) {
    kprintf("[kvm_driver] Initializing KVM host driver\n");
    
    /* Initialize global KVM host state */
    memset(&g_kvm_host_state, 0, sizeof(g_kvm_host_state));
    
    /* Try to open /dev/kvm */
    g_kvm_host_state.dev_kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (g_kvm_host_state.dev_kvm_fd < 0) {
        kprintf("[kvm_driver] WARNING: /dev/kvm not available, KVM host support disabled\n");
        return;
    }
    
    /* Get KVM API version */
    if (ioctl(g_kvm_host_state.dev_kvm_fd, KVM_GET_API_VERSION, &g_kvm_host_state.api_version) < 0) {
        kprintf("[kvm_driver] WARNING: KVM_GET_API_VERSION failed\n");
        close(g_kvm_host_state.dev_kvm_fd);
        g_kvm_host_state.dev_kvm_fd = -1;
        return;
    }
    
    /* Allocate VM array */
    g_kvm_host_state.vms = (kvm_vm_context_t *)kmalloc(sizeof(kvm_vm_context_t) * 16);
    if (g_kvm_host_state.vms) {
        g_kvm_host_state.max_vms = 16;
        memset(g_kvm_host_state.vms, 0, sizeof(kvm_vm_context_t) * 16);
    }
    
    g_kvm_host_state.initialized = true;
    kprintf("[kvm_driver] KVM host driver initialized (API version %d)\n", g_kvm_host_state.api_version);
}

/* Driver cleanup function */
static void kvm_driver_cleanup(void) {
    if (!g_kvm_host_state.initialized) {
        return;
    }
    
    /* Clean up all VMs */
    for (uint32_t i = 0; i < g_kvm_host_state.vm_count; i++) {
        kvm_vm_destroy(&g_kvm_host_state.vms[i]);
    }
    
    /* Free VM array */
    if (g_kvm_host_state.vms) {
        kfree(g_kvm_host_state.vms);
        g_kvm_host_state.vms = NULL;
    }
    
    /* Close /dev/kvm */
    if (g_kvm_host_state.dev_kvm_fd >= 0) {
        close(g_kvm_host_state.dev_kvm_fd);
        g_kvm_host_state.dev_kvm_fd = -1;
    }
    
    g_kvm_host_state.initialized = false;
    kprintf("[kvm_driver] KVM host driver cleaned up\n");
}

/* KVM host driver implementation */
int kvm_init(void) {
    return kvm_host_init();
}

int kvm_create_vm(kvm_t *kvm) {
    if (!kvm || !g_kvm_host_state.initialized) {
        return -1;
    }
    
    /* Create VM */
    kvm->vm_fd = ioctl(g_kvm_host_state.dev_kvm_fd, KVM_CREATE_VM, 0);
    if (kvm->vm_fd < 0) {
        return -1;
    }
    
    /* Initialize VM context */
    memset(kvm, 0, sizeof(kvm_t));
    kvm->vm_fd = kvm->vm_fd; /* Store VM fd */
    
    return 0;
}

int kvm_create_vcpu(kvm_t *kvm, int *vcpu_fd) {
    if (!kvm || !vcpu_fd || kvm->vm_fd < 0) {
        return -1;
    }
    
    /* Create VCPU */
    *vcpu_fd = ioctl(kvm->vm_fd, KVM_CREATE_VCPU, 0);
    if (*vcpu_fd < 0) {
        return -1;
    }
    
    return 0;
}

int kvm_set_user_memory_region(kvm_t *kvm, uint64_t slot,
                               uint64_t guest_phys_addr,
                               uint64_t memory_size,
                               uint64_t userspace_addr) {
    if (!kvm || kvm->vm_fd < 0) {
        return -1;
    }
    
    struct kvm_userspace_memory_region region;
    memset(&region, 0, sizeof(region));
    region.slot = slot;
    region.guest_phys_addr = guest_phys_addr;
    region.memory_size = memory_size;
    region.userspace_addr = userspace_addr;
    
    if (ioctl(kvm->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        return -1;
    }
    
    return 0;
}

int kvm_vcpu_run(int vcpu_fd) {
    struct kvm_run run;
    memset(&run, 0, sizeof(run));
    
    int ret = ioctl(vcpu_fd, KVM_RUN, &run);
    if (ret < 0) {
        return -1;
    }
    
    return run.exit_reason;
}

void kvm_close(kvm_t *kvm) {
    if (!kvm) return;
    
    if (kvm->vcpu_fd > 0) {
        close(kvm->vcpu_fd);
        kvm->vcpu_fd = -1;
    }
    
    if (kvm->vm_fd > 0) {
        close(kvm->vm_fd);
        kvm->vm_fd = -1;
    }
}

/* Driver registration */
void register_kvm_driver(void) {
    g_kvm_driver.name = "kvm_host";
    g_kvm_driver.init = kvm_driver_init;
    g_kvm_driver.probe = NULL;
    g_kvm_driver.remove = NULL;
    
    driver_register(&g_kvm_driver);
}

void deregister_kvm_driver(void) {
    kvm_driver_cleanup();
}

#ifdef __cplusplus
}
#endif

#endif /* kvm_driver.c */