/* kvm_driver.c — KVM virtualization host driver.
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

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <unistd.h>

#include "kvm_driver.h"

/* Driver logging (kernel console) */
extern void kprintf(const char *fmt, ...);

#ifdef __cplusplus
extern "C" {
#endif

/* Driver state */
typedef struct {
    int fd;            /* /dev/kvm file descriptor */
    uint32_t api_version;
    bool initialized;
} kvm_context_t;

/* Global KVM state */
static kvm_context_t g_kvm_context = {0};

/* Driver initialization function */
static void kvm_driver_init(void) {
    kprintf("[kvm_driver] Initializing KVM host driver...\n");

    /* Try to open /dev/kvm */
    g_kvm_context.fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (g_kvm_context.fd < 0) {
        kprintf("[kvm_driver] WARNING: /dev/kvm not available - KVM host support disabled\n");
        return;
    }

    /* Get KVM API version */
    if (ioctl(g_kvm_context.fd, KVM_GET_API_VERSION, &g_kvm_context.api_version) < 0) {
        kprintf("[kvm_driver] WARNING: Failed to get KVM API version\n");
        close(g_kvm_context.fd);
        g_kvm_context.fd = -1;
        return;
    }

    g_kvm_context.initialized = true;
    kprintf("[kvm_driver] KVM host driver initialized (API version %d)\n", g_kvm_context.api_version);
}

/* Driver cleanup function */
static void kvm_driver_cleanup(void) {
    if (!g_kvm_context.initialized) {
        return;
    }

    if (g_kvm_context.fd >= 0) {
        close(g_kvm_context.fd);
        g_kvm_context.fd = -1;
    }

    g_kvm_context.initialized = false;
    kprintf("[kvm_driver] KVM host driver cleaned up\n");
}

/* KVM API implementations */
void kvm_init(void) {
    kvm_driver_init();
}

int kvm_create_vm(kvm_t *kvm) {
    if (!kvm || !g_kvm_context.initialized || g_kvm_context.fd < 0) {
        return -1;
    }

    /* Create VM */
    kvm->vm_fd = ioctl(g_kvm_context.fd, KVM_CREATE_VM, 0);
    if (kvm->vm_fd < 0) {
        return -1;
    }

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

/* Driver registration function */
void register_kvm_driver(void) {
}

void deregister_kvm_driver(void) {
    kvm_driver_cleanup();
}

#ifdef __cplusplus
}
#endif