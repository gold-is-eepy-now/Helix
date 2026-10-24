/* usermode.c — minimal ring-3 entry/exit proof before ELF and syscall work. */
#include "usermode.h"

#include <stddef.h>

#include "console.h"
#include "channel.h"
#include "editor.h"
#include "fat32.h"
#include "driver.h"
#include "ehci.h"
#include "gdt.h"
#include "hfs.h"
#include "install.h"
#include "io.h"
#include "input.h"
#include "kernel_stack.h"
#include "kstring.h"
#include "pmm.h"
#include "pci.h"
#include "syscall.h"
#include "sync.h"
#include "thread.h"
#include "sysinfo.h"
#include "vmm.h"
#include "version.h"
#include "xhci.h"

extern bool usb_storage_register_if_ready(void);

static uint32_t usb_storage_sector_size(void) {
    if (xhci_storage_ready()) return xhci_sector_size();
    if (ehci_storage_ready()) return ehci_sector_size();
    return 0;
}

#define USER_CODE_VIRT  0x00400000ULL
#define USER_STACK_VIRT 0x00401000ULL
#define USER_STACK_TOP  (USER_STACK_VIRT + PAGE_SIZE)
#define USER_EXIT_VECTOR 128u
#define TRANSITION_STACK_FIRST_GUARD 0xffff900000400000ULL
#define TRANSITION_STACK_SLOT_SIZE 0x10000ULL
#define TRANSITION_STACK_PAGES 8u
#define HFS_USER_MAX_FILE_SIZE 8192u

uint64_t usermode_saved_rsp;
static bool g_exit_armed;
static bool g_exit_received;
static uint64_t g_result;
static uint64_t g_fault_vector;
static uintptr_t g_active_transition_stack_phys;
static uint64_t g_active_transition_stack_guard;
static uint64_t g_active_transition_stack_base;
static uint64_t g_active_transition_stack_top;
static uint64_t g_next_transition_stack_guard = TRANSITION_STACK_FIRST_GUARD;
static vmm_address_space_t *g_active_space;
static handle_table_t g_process_handles;
static handle_t g_process_uart_capability;
static handle_t g_process_usb_request_channel;
static handle_t g_process_usb_response_channel;
static bool g_grant_uart_on_next_run;
static bool g_grant_usb_read_on_next_run;
static handle_table_t g_usb_service_handles;
static handle_table_t g_usb_client_template;
static handle_t g_usb_service_request_receive;
static handle_t g_usb_service_response_send;
static handle_t g_usb_service_read_capability;
static handle_t g_usb_service_write_capability;
static handle_t g_usb_client_request_send;
static handle_t g_usb_client_response_receive;
static bool g_usb_service_ready;
typedef struct {
    uint32_t operation; uint32_t reserved; uint64_t lba; uint8_t data[512];
}
    usb_service_request_t;
typedef struct {
    int64_t status;
    uint32_t length;
    uint8_t data[512];
} usb_service_response_t;
typedef struct { uint64_t kernel_rsp; uint64_t user_rsp; } syscall_cpu_t;
static syscall_cpu_t g_syscall_cpu __attribute__((aligned(16)));

static void usb_storage_service_worker(void *argument) {
    (void)argument;
    for (;;) {
        usb_service_request_t request;
        size_t request_size = 0;
        channel_result_t received = channel_receive_wait(&g_usb_service_handles,
            g_usb_service_request_receive, &request, sizeof(request),
            &request_size);
        if (received != CHANNEL_OK || request_size != sizeof(request)) {
            thread_yield();
            continue;
        }
        usb_service_response_t response = { .status = -1, .length = 0 };
        if (request.operation == 1 && usb_storage_sector_size() == sizeof(response.data) &&
            driver_read(&g_usb_service_handles, g_usb_service_read_capability,
                        request.lba, response.data, sizeof(response.data))) {
            response.status = 0;
            response.length = sizeof(response.data);
        } else if (request.operation == 2 &&
            usb_storage_sector_size() == sizeof(request.data) &&
            driver_write(&g_usb_service_handles,
                g_usb_service_write_capability, request.lba, request.data,
                sizeof(request.data))) {
            response.status = 0;
            response.length = 0;
        }
        (void)channel_send(&g_usb_service_handles, g_usb_service_response_send,
                           &response, sizeof(response));
    }
}

static bool usb_service_start(void) {
    if (g_usb_service_ready) return true;
    driver_info_t storage = {0};
    bool found = false;
    for (size_t i = 0; i < driver_count(); i++) {
        if (driver_get_info(i, &storage) &&
            strcmp(storage.name, "usb-msd") == 0 && storage.read_handle != 0 &&
            storage.write_handle != 0) {
            found = true;
            break;
        }
    }
    if (!found) return false;

    handle_table_init(&g_usb_service_handles);
    handle_table_init(&g_usb_client_template);
    if (!channel_create_pair_between(&g_usb_client_template,
            &g_usb_service_handles, 2, &g_usb_client_request_send,
            &g_usb_service_request_receive) ||
        !channel_create_pair_between(&g_usb_service_handles,
            &g_usb_client_template, 2, &g_usb_service_response_send,
            &g_usb_client_response_receive))
        goto fail;

    object_t *capability = handle_get(object_kernel_handles(),
        storage.read_handle, OBJECT_RIGHT_READ, OBJECT_TYPE_CAPABILITY);
    if (capability == NULL) goto fail;
    g_usb_service_read_capability = handle_create(&g_usb_service_handles,
        capability, OBJECT_RIGHT_READ);
    object_release(capability);
    if (g_usb_service_read_capability == 0) goto fail;
    capability = handle_get(object_kernel_handles(), storage.write_handle,
        OBJECT_RIGHT_READ, OBJECT_TYPE_CAPABILITY);
    if (capability == NULL) goto fail;
    g_usb_service_write_capability = handle_create(&g_usb_service_handles,
        capability, OBJECT_RIGHT_WRITE);
    object_release(capability);
    if (g_usb_service_write_capability == 0) goto fail;

    g_usb_service_ready = true;
    if (thread_create(usb_storage_service_worker, NULL) == 0) {
        g_usb_service_ready = false;
        goto fail;
    }
    return true;

fail:
    handle_table_close_all(&g_usb_service_handles);
    handle_table_close_all(&g_usb_client_template);
    g_usb_service_request_receive = 0;
    g_usb_service_response_send = 0;
    g_usb_service_read_capability = 0;
    g_usb_service_write_capability = 0;
    g_usb_client_request_send = 0;
    g_usb_client_response_receive = 0;
    return false;
}

static bool usb_service_grant_process(void) {
    if (!usb_service_start()) return false;
    object_t *request = handle_get(&g_usb_client_template,
        g_usb_client_request_send, OBJECT_RIGHT_WRITE, OBJECT_TYPE_CHANNEL);
    if (request == NULL) return false;
    g_process_usb_request_channel = handle_create(&g_process_handles, request,
                                                  OBJECT_RIGHT_WRITE);
    object_release(request);
    object_t *response = handle_get(&g_usb_client_template,
        g_usb_client_response_receive, OBJECT_RIGHT_READ, OBJECT_TYPE_CHANNEL);
    if (response == NULL) return false;
    g_process_usb_response_channel = handle_create(&g_process_handles, response,
                                                    OBJECT_RIGHT_READ);
    object_release(response);
    return g_process_usb_request_channel != 0 &&
           g_process_usb_response_channel != 0;
}

extern void usermode_enter(uint64_t entry, uint64_t user_rsp);
extern void usermode_kernel_return(void);
extern void usermode_syscall_entry(void);

void usermode_grant_next_uart_control(void) {
    IRQ_GUARD;
    g_grant_uart_on_next_run = true;
}

void usermode_grant_next_usb_read(void) {
    IRQ_GUARD;
    g_grant_usb_read_on_next_run = true;
}

static bool usermode_prepare_handles(void) {
    handle_table_init(&g_process_handles);
    g_process_uart_capability = 0;
    g_process_usb_request_channel = 0;
    g_process_usb_response_channel = 0;
    if (!g_grant_uart_on_next_run && !g_grant_usb_read_on_next_run) return true;
    bool grant_uart = g_grant_uart_on_next_run;
    bool grant_usb = g_grant_usb_read_on_next_run;
    g_grant_uart_on_next_run = false;
    g_grant_usb_read_on_next_run = false;

    handle_table_t *kernel_handles = object_kernel_handles();
    bool uart_found = !grant_uart;
    bool usb_storage_available = xhci_storage_ready() || ehci_storage_ready();
    bool usb_found = !grant_usb || !usb_storage_available;
    for (size_t i = 0; i < driver_count(); i++) {
        driver_info_t info;
        if (!driver_get_info(i, &info)) continue;
        if (grant_uart && strcmp(info.name, "uart16550") == 0) {
            object_t *capability = handle_get(kernel_handles, info.control_handle,
                OBJECT_RIGHT_READ, OBJECT_TYPE_CAPABILITY);
            if (capability == NULL) return false;
            g_process_uart_capability = handle_create(&g_process_handles,
                capability, OBJECT_RIGHT_READ);
            object_release(capability);
            if (g_process_uart_capability == 0) return false;
            uart_found = true;
        }
    }
    if (grant_usb && usb_storage_available)
        usb_found = usb_service_grant_process();
    return uart_found && usb_found;
}

static void usermode_release_handles(void) {
    handle_table_close_all(&g_process_handles);
    g_process_uart_capability = 0;
    g_process_usb_request_channel = 0;
    g_process_usb_response_channel = 0;
}

bool usermode_syscall_init(void) {
    const uint32_t cpuid_leaf = 0x80000001u;
    uint32_t max_ext, b, c, d;
    __asm__ volatile("cpuid" : "=a"(max_ext), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(0x80000000u), "c"(0));
    if (max_ext < cpuid_leaf) return false;
    __asm__ volatile("cpuid" : "=a"(max_ext), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(cpuid_leaf), "c"(0));
    if ((d & (1u << 11)) == 0) return false; /* SYSCALL/SYSRET */

    const uint32_t efer_msr = 0xC0000080u;
    const uint32_t star_msr = 0xC0000081u;
    const uint32_t lstar_msr = 0xC0000082u;
    const uint32_t fmask_msr = 0xC0000084u;
    const uint32_t gs_base_msr = 0xC0000101u;        /* live GS base         */
    const uint32_t kernel_gs_base_msr = 0xC0000102u; /* swapped in by SWAPGS */
    wrmsr(efer_msr, rdmsr(efer_msr) | 1u); /* EFER.SCE */
    wrmsr(star_msr, ((uint64_t)0x13u << 48) | ((uint64_t)0x08u << 32));
    wrmsr(lstar_msr, (uint64_t)(uintptr_t)usermode_syscall_entry);
    wrmsr(fmask_msr, (1u << 8) | (1u << 9) | (1u << 10)); /* TF, IF, DF */
    g_syscall_cpu.kernel_rsp = KERNEL_STACK_TOP;
    g_syscall_cpu.user_rsp = 0;
    /* Per-CPU data is reached through SWAPGS on syscall entry, so user code
     * cannot redirect it by loading its own GS (that resets only the live
     * base, which is the user's). */
    wrmsr(gs_base_msr, 0);
    wrmsr(kernel_gs_base_msr, (uint64_t)(uintptr_t)&g_syscall_cpu);
    return true;
}

static void redirect_to_kernel(registers_t *frame) {
    frame->rip = (uint64_t)(uintptr_t)usermode_kernel_return;
    frame->cs = GDT_KERNEL_CODE_SELECTOR;
    frame->rflags = 0x2;
    frame->rsp = g_active_transition_stack_top;
    frame->ss = GDT_KERNEL_DATA_SELECTOR;
}

static bool create_transition_stack(void) {
    if (g_active_transition_stack_phys != 0) return false;
    uint64_t guard = g_next_transition_stack_guard;
    uint64_t base = guard + PAGE_SIZE;
    for (uint64_t page = 0; page <= TRANSITION_STACK_PAGES; page++) {
        if (vmm_get_mapping(guard + page * PAGE_SIZE,
                            NULL, NULL)) return false;
    }
    uintptr_t phys = pmm_alloc_frames(TRANSITION_STACK_PAGES);
    if (phys == 0) return false;
    uint64_t mapped = 0;
    for (; mapped < TRANSITION_STACK_PAGES; mapped++) {
        uint64_t virt = base + mapped * PAGE_SIZE;
        uintptr_t frame = phys + mapped * PAGE_SIZE;
        if (!vmm_map(virt, frame, VMM_PRESENT | VMM_WRITE | VMM_NX)) break;
        memset((void *)(uintptr_t)virt, 0, PAGE_SIZE);
    }
    if (mapped != TRANSITION_STACK_PAGES) {
        while (mapped != 0) {
            mapped--;
            vmm_unmap(base + mapped * PAGE_SIZE);
        }
        pmm_free_frames(phys, TRANSITION_STACK_PAGES);
        return false;
    }
    g_active_transition_stack_phys = phys;
    g_active_transition_stack_guard = guard;
    g_active_transition_stack_base = base;
    g_active_transition_stack_top = base + TRANSITION_STACK_PAGES * PAGE_SIZE;
    g_next_transition_stack_guard += TRANSITION_STACK_SLOT_SIZE;
    return true;
}

static void destroy_transition_stack(void) {
    if (g_active_transition_stack_phys == 0) return;
    for (uint64_t page = 0; page < TRANSITION_STACK_PAGES; page++)
        vmm_unmap(g_active_transition_stack_base + page * PAGE_SIZE);
    pmm_free_frames(g_active_transition_stack_phys, TRANSITION_STACK_PAGES);
    g_active_transition_stack_phys = 0;
    g_active_transition_stack_guard = 0;
    g_active_transition_stack_base = 0;
    g_active_transition_stack_top = 0;
}

static bool user_pointer_accessible(uint64_t address, uint64_t length,
                                    uint64_t required_flags) {
    const uint64_t user_top = 0x0000800000000000ULL;
    if (length == 0) return true;
    if (address < PAGE_SIZE || address >= user_top ||
        length > user_top - address) return false;
    uint64_t last = address + length - 1;
    for (uint64_t page = address & ~((uint64_t)PAGE_SIZE - 1);;
         page += PAGE_SIZE) {
        uint64_t flags = 0;
        if (!vmm_address_space_get_mapping(g_active_space, page, NULL,
                                           &flags) ||
            (flags & required_flags) != required_flags)
            return false;
        if (page == (last & ~((uint64_t)PAGE_SIZE - 1))) break;
    }
    return true;
}

static bool copy_user_hfs_path(uint64_t address, char path[40]) {
    if (address == 0 || path == NULL) return false;
    for (uint64_t i = 0; i < 40; i++) {
        if (address > UINT64_MAX - i ||
            !user_pointer_accessible(address + i, 1, VMM_PRESENT | VMM_USER))
            return false;
        path[i] = *(const char *)(uintptr_t)(address + i);
        if (path[i] == '\0') return i != 0;
    }
    return false;
}

bool usermode_handle_syscall(registers_t *frame) {
    if (frame == NULL || frame->vector != USER_EXIT_VECTOR ||
        (frame->cs & 3u) != 3u || !g_exit_armed || g_active_space == NULL)
        return false;

    if (frame->rax == HELIX_SYS_WRITE) {
        uint64_t address = frame->rdi;
        uint64_t length = frame->rsi;
        if (length > HELIX_SYS_MAX_WRITE ||
            !user_pointer_accessible(address, length,
                                     VMM_PRESENT | VMM_USER)) {
            frame->rax = (uint64_t)-1;
            return true;
        }
        const char *text = (const char *)(uintptr_t)address;
        for (uint64_t i = 0; i < length; i++) console_putc(text[i]);
        frame->rax = length;
        return true;
    }
    if (frame->rax == HELIX_SYS_READ) {
        uint64_t address = frame->rdi;
        uint64_t capacity = frame->rsi;
        if (capacity > HELIX_SYS_MAX_READ ||
            !user_pointer_accessible(address, capacity,
                VMM_PRESENT | VMM_USER | VMM_WRITE)) {
            frame->rax = (uint64_t)-1;
            return true;
        }
        char *buffer = (char *)(uintptr_t)address;
        uint64_t length = 0;
        while (length < capacity) {
            int c = input_getchar();
            if (c < 0) {
                __asm__ volatile("pause");
                continue;
            }
            if (c == '\r') c = '\n';
            if ((c == '\b' || c == 0x7f) && length != 0) {
                length--;
                console_backspace();
                continue;
            }
            if (c == '\b' || c == 0x7f) continue;
            buffer[length++] = (char)c;
            console_putc((char)c);
            if (c == '\n') break;
        }
        frame->rax = length;
        return true;
    }
    if (frame->rax == HELIX_SYS_CLEAR) {
        console_clear();
        frame->rax = 0;
        return true;
    }
    if (frame->rax == HELIX_SYS_VERSION) {
        console_write(HELIX_NAME);
        console_write(" ");
        console_write(HELIX_VERSION);
        console_write(" (user shell)\n");
        frame->rax = 0;
        return true;
    }
    if (frame->rax == HELIX_SYS_SYSINFO) {
        sysinfo_print();
        frame->rax = 0;
        return true;
    }
    if (frame->rax == HELIX_SYS_EDIT) {
        if (frame->rdi == 0) {
            editor_run();
            frame->rax = 0;
        } else {
            char path[40];
            bool opened = copy_user_hfs_path(frame->rdi, path) &&
                          editor_run_file(path);
            frame->rax = opened ? 0 : (uint64_t)-1;
        }
        return true;
    }
    if (frame->rax == HELIX_SYS_DRIVERS) {
        console_write("Registered device drivers:\n");
        for (size_t i = 0; i < driver_count(); i++) {
            driver_info_t info;
            if (!driver_get_info(i, &info)) continue;
            console_write("  ");
            console_write(info.name);
            console_write("  class=");
            console_write(info.device_class);
            if (info.sector_size != 0 && info.sector_count != 0) {
                console_write("  ");
                console_write_dec(info.sector_size);
                console_write("-byte sectors x ");
                console_write_dec(info.sector_count);
            }
            console_write(info.active ? "  active\n" : "  discovered\n");
        }
        frame->rax = 0;
        return true;
    }
    if (frame->rax == HELIX_SYS_UART_GET_HANDLE) {
        frame->rax = g_process_uart_capability != 0
            ? g_process_uart_capability : (uint64_t)-1;
        return true;
    }
    if (frame->rax == HELIX_SYS_USB_INFO) {
        pci_print_usb_controllers();
        if ((xhci_storage_ready() || ehci_storage_ready()) &&
            usb_storage_register_if_ready() &&
            g_process_usb_request_channel == 0 &&
            !usb_service_grant_process())
            console_write("[helix] usb: storage service setup failed\n");
        frame->rax = 0;
        return true;
    }
    if (frame->rax == HELIX_SYS_USB_GET_SERVICE_CHANNEL) {
        frame->rax = g_process_usb_request_channel != 0
            ? g_process_usb_request_channel : (uint64_t)-1;
        return true;
    }
    if (frame->rax == HELIX_SYS_USB_READ_SECTOR) {
        uint64_t capability = frame->rdi;
        uint64_t lba = frame->rsi;
        uint64_t address = frame->rdx;
        uint32_t sector_size = usb_storage_sector_size();
        bool valid = capability <= UINT32_MAX && sector_size != 0 &&
            sector_size <= PAGE_SIZE &&
            user_pointer_accessible(address, sector_size,
                VMM_PRESENT | VMM_USER | VMM_WRITE);
        bool matching_channel = capability == g_process_usb_request_channel &&
            g_process_usb_request_channel != 0 &&
            g_process_usb_response_channel != 0;
        usb_service_request_t request = { .operation = 1, .lba = lba };
        usb_service_response_t response;
        size_t response_size = 0;
        channel_result_t sent = valid && matching_channel
            ? channel_send(&g_process_handles,
                g_process_usb_request_channel, &request, sizeof(request))
            : CHANNEL_DENIED;
        channel_result_t received = sent == CHANNEL_OK
            ? channel_receive_wait(&g_process_handles,
                g_process_usb_response_channel, &response, sizeof(response),
                &response_size)
            : CHANNEL_DENIED;
        bool read = received == CHANNEL_OK &&
            response_size == sizeof(response) && response.status == 0 &&
            response.length == sector_size;
        if (read) memcpy((void *)(uintptr_t)address, response.data, sector_size);
        frame->rax = read ? 0 : (uint64_t)-1;
        return true;
    }
    if (frame->rax == HELIX_SYS_USB_WRITE_SECTOR) {
        uint64_t capability = frame->rdi;
        uint64_t lba = frame->rsi;
        uint64_t address = frame->rdx;
        bool valid = capability <= UINT32_MAX &&
            usb_storage_sector_size() == 512 &&
            user_pointer_accessible(address, 512, VMM_PRESENT | VMM_USER);
        bool matching_channel = capability == g_process_usb_request_channel &&
            g_process_usb_request_channel != 0 &&
            g_process_usb_response_channel != 0;
        usb_service_request_t request = { .operation = 2, .lba = lba };
        usb_service_response_t response;
        size_t response_size = 0;
        if (valid && matching_channel)
            memcpy(request.data, (const void *)(uintptr_t)address, sizeof(request.data));
        channel_result_t sent = valid && matching_channel
            ? channel_send(&g_process_handles, g_process_usb_request_channel,
                          &request, sizeof(request)) : CHANNEL_DENIED;
        channel_result_t received = sent == CHANNEL_OK
            ? channel_receive_wait(&g_process_handles,
                g_process_usb_response_channel, &response, sizeof(response),
                &response_size) : CHANNEL_DENIED;
        bool written = received == CHANNEL_OK &&
            response_size == sizeof(response) && response.status == 0;
        frame->rax = written ? 0 : (uint64_t)-1;
        return true;
    }
    if (frame->rax == HELIX_SYS_HFS_LIST) {
        bool listed = hfs_print_usb0_list();
        frame->rax = listed ? 0 : (uint64_t)-1;
        return true;
    }
    if (frame->rax == HELIX_SYS_HFS_CAT) {
        char name[40];
        uint64_t address = frame->rdi;
        bool valid = user_pointer_accessible(address, sizeof(name),
                                             VMM_PRESENT | VMM_USER);
        size_t length = 0;
        if (valid) {
            const char *user_name = (const char *)(uintptr_t)address;
            while (length < sizeof(name) && user_name[length] != '\0') {
                name[length] = user_name[length];
                length++;
            }
            valid = length != 0 && length < sizeof(name);
            if (valid) name[length] = '\0';
        }
        frame->rax = valid && hfs_print_usb0_file(name)
            ? 0 : (uint64_t)-1;
        return true;
    }
    if (frame->rax == HELIX_SYS_HFS_READ) {
        char path[40];
        uint64_t user_buffer = frame->rsi;
        uint64_t capacity = frame->rdx;
        char contents[HFS_USER_MAX_FILE_SIZE];
        unsigned long length = 0;
        bool valid = capacity <= HFS_USER_MAX_FILE_SIZE &&
            copy_user_hfs_path(frame->rdi, path);
        bool read = valid && hfs_read_file(path, contents, capacity, &length);
        if (read && user_pointer_accessible(user_buffer, length,
                VMM_PRESENT | VMM_USER | VMM_WRITE))
            memcpy((void *)(uintptr_t)user_buffer, contents, length);
        else
            read = false;
        frame->rax = read ? length : (uint64_t)-1;
        return true;
    }
    if (frame->rax == HELIX_SYS_HFS_WRITE) {
        char path[40];
        uint64_t user_data = frame->rsi;
        uint64_t length = frame->rdx;
        char contents[HFS_USER_MAX_FILE_SIZE];
        bool valid = length <= HFS_USER_MAX_FILE_SIZE &&
            copy_user_hfs_path(frame->rdi, path) &&
            user_pointer_accessible(user_data, length, VMM_PRESENT | VMM_USER);
        if (valid && length != 0)
            memcpy(contents, (const void *)(uintptr_t)user_data, length);
        frame->rax = valid && hfs_write_file(path, contents, length)
            ? 0 : (uint64_t)-1;
        return true;
    }
    if (frame->rax == HELIX_SYS_HFS_DELETE) {
        char path[40];
        bool deleted = copy_user_hfs_path(frame->rdi, path) &&
                       hfs_delete_file(path);
        frame->rax = deleted ? 0 : (uint64_t)-1;
        return true;
    }
    if (frame->rax == HELIX_SYS_FAT32_DELETE) {
        char name[40];
        bool deleted = copy_user_hfs_path(frame->rdi, name) &&
                       fat32_delete_usb0_file(name);
        frame->rax = deleted ? 0 : (uint64_t)-1;
        return true;
    }
    if (frame->rax == HELIX_SYS_FAT32_WRITE) {
        char name[40];
        char contents[8192];
        uint64_t user_data = frame->rsi;
        uint64_t length = frame->rdx;
        bool valid = length <= sizeof(contents) &&
            copy_user_hfs_path(frame->rdi, name) &&
            user_pointer_accessible(user_data, length, VMM_PRESENT | VMM_USER);
        if (valid && length != 0)
            memcpy(contents, (const void *)(uintptr_t)user_data, length);
        frame->rax = valid && fat32_save_usb0_file(name, contents, length)
            ? 0 : (uint64_t)-1;
        return true;
    }
    if (frame->rax == HELIX_SYS_HFS_FORMAT) {
        char device[40];
        bool formatted = copy_user_hfs_path(frame->rdi, device) &&
                         hfs_format_device(device);
        frame->rax = formatted ? 0 : (uint64_t)-1;
        return true;
    }
    if (frame->rax == HELIX_SYS_INSTALL) {
        char device[40];
        bool installed = copy_user_hfs_path(frame->rdi, device) &&
                         install_to_disk(device);
        frame->rax = installed ? 0 : (uint64_t)-1;
        return true;
    }
    if (frame->rax == HELIX_SYS_UART_WRITE_BYTE) {
        uint64_t capability = frame->rdi;
        uint64_t byte = frame->rsi;
        bool sent = capability <= UINT32_MAX && byte <= 0xffu &&
            byte != '\n' && byte != '\r' &&
            driver_control(&g_process_handles, (handle_t)capability,
                           DRIVER_CONTROL_UART_WRITE_BYTE, byte);
        frame->rax = sent ? 0 : (uint64_t)-1;
        return true;
    }
    if (frame->rax == HELIX_SYS_UART_GET_HANDLE) {
        frame->rax = g_process_uart_capability != 0
            ? g_process_uart_capability : (uint64_t)-1;
        return true;
    }
    if (frame->rax != HELIX_SYS_EXIT) {
        frame->rax = (uint64_t)-1;
        return true;
    }

    g_result = (uint32_t)frame->rdi;
    g_exit_received = true;
    g_exit_armed = false;
    if (!vmm_address_space_switch(vmm_kernel_address_space())) return false;
    g_active_space = NULL;

    /* IRETQ returns to a kernel trampoline, which restores the saved call
     * stack; same-privilege return does not consume the RSP/SS pair. */
    redirect_to_kernel(frame);
    return true;
}

bool usermode_test(uint64_t *result_out) {
    if (result_out == NULL) return false;
    IRQ_GUARD;

    vmm_address_space_t space = {0};
    uintptr_t code_frame = 0;
    uintptr_t stack_frame = 0;
    bool code_mapped = false;
    bool stack_mapped = false;
    bool entered = false;
    bool ok = vmm_address_space_create(&space);
    if (!ok) goto cleanup;

    code_frame = pmm_alloc_frame();
    stack_frame = pmm_alloc_frame();
    if (code_frame == 0 || stack_frame == 0) {
        ok = false;
        goto cleanup;
    }
    code_mapped = vmm_address_space_map_user(&space, USER_CODE_VIRT,
        code_frame, VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
    stack_mapped = vmm_address_space_map_user(&space, USER_STACK_VIRT,
        stack_frame, VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
    if (!code_mapped || !stack_mapped) {
        ok = false;
        goto cleanup;
    }

    /* mov eax, SYS_EXIT; mov edi, 42; int 0x80; ud2. The DPL-3 gate
     * dispatches the first Helix syscall and redirects back to the kernel.
     */
    static const uint8_t user_program[] = {
        0xB8, 0x00, 0x00, 0x00, 0x00,
        0xBF, 0x2A, 0x00, 0x00, 0x00,
        0xCD, 0x80,
        0x0F, 0x0B,
    };
    if (!vmm_address_space_switch(&space)) {
        ok = false;
        goto cleanup;
    }
    memcpy((void *)(uintptr_t)USER_CODE_VIRT, user_program,
           sizeof(user_program));
    memset((void *)(uintptr_t)USER_STACK_VIRT, 0, PAGE_SIZE);
    bool code_protected = vmm_protect(USER_CODE_VIRT,
                                      VMM_PRESENT | VMM_USER);
    if (!vmm_address_space_switch(vmm_kernel_address_space())) {
        ok = false;
        goto cleanup;
    }
    if (!code_protected) {
        ok = false;
        goto cleanup;
    }

    entered = usermode_run(&space, USER_CODE_VIRT, USER_STACK_TOP - 8,
                           result_out, NULL);
    ok = entered && *result_out == 42;

cleanup:
    g_exit_armed = false;
    if ((read_cr3() & 0x000FFFFFFFFFF000ULL) !=
        (vmm_kernel_address_space()->root_phys & 0x000FFFFFFFFFF000ULL)) {
        vmm_address_space_switch(vmm_kernel_address_space());
    }
    if (code_mapped) vmm_address_space_unmap_user(&space, USER_CODE_VIRT);
    if (stack_mapped) vmm_address_space_unmap_user(&space, USER_STACK_VIRT);
    if (space.root_phys != 0) vmm_address_space_destroy(&space);
    if (code_frame != 0) pmm_free_frame(code_frame);
    if (stack_frame != 0) pmm_free_frame(stack_frame);
    return entered && ok;
}

bool usermode_run(vmm_address_space_t *space, uint64_t entry,
                 uint64_t user_rsp, uint64_t *result_out,
                 uint64_t *fault_vector_out) {
    IRQ_GUARD;
    if (space == NULL || space->root_phys == 0 || entry == 0 ||
        user_rsp == 0 || result_out == NULL || !usermode_prepare_handles())
        return false;
    if (!create_transition_stack()) {
        usermode_release_handles();
        return false;
    }

    g_result = 0;
    g_fault_vector = 0;
    g_exit_received = false;
    g_exit_armed = true;
    g_syscall_cpu.kernel_rsp = g_active_transition_stack_top;
    if (!gdt_set_rsp0(g_active_transition_stack_top) ||
        !vmm_address_space_switch(space)) {
        g_exit_armed = false;
        gdt_set_rsp0(KERNEL_STACK_TOP);
        g_syscall_cpu.kernel_rsp = KERNEL_STACK_TOP;
        destroy_transition_stack();
        usermode_release_handles();
        return false;
    }

    g_active_space = space;
    usermode_enter(entry, user_rsp);
    g_exit_armed = false;
    if ((read_cr3() & 0x000FFFFFFFFFF000ULL) !=
        (vmm_kernel_address_space()->root_phys & 0x000FFFFFFFFFF000ULL) &&
        !vmm_address_space_switch(vmm_kernel_address_space())) {
        g_active_space = NULL;
        gdt_set_rsp0(KERNEL_STACK_TOP);
        g_syscall_cpu.kernel_rsp = KERNEL_STACK_TOP;
        destroy_transition_stack();
        usermode_release_handles();
        return false;
    }
    g_active_space = NULL;
    gdt_set_rsp0(KERNEL_STACK_TOP);
    g_syscall_cpu.kernel_rsp = KERNEL_STACK_TOP;
    destroy_transition_stack();
    usermode_release_handles();
    if (!g_exit_received) return false;
    *result_out = g_result;
    if (fault_vector_out != NULL) *fault_vector_out = g_fault_vector;
    return true;
}

bool usermode_handle_exception(registers_t *frame) {
    if (frame == NULL || frame->vector >= 32 || (frame->cs & 3u) != 3u ||
        !g_exit_armed || g_active_space == NULL) return false;

    g_fault_vector = frame->vector;
    g_exit_received = true;
    g_exit_armed = false;
    if (!vmm_address_space_switch(vmm_kernel_address_space())) return false;
    g_active_space = NULL;
    redirect_to_kernel(frame);
    return true;
}

bool usermode_fault_test(uint64_t *fault_vector_out) {
    IRQ_GUARD;
    const uint64_t stack_page = 0x00401000ULL;
    const uint64_t stack_top = stack_page + PAGE_SIZE;
    vmm_address_space_t space = {0};
    uintptr_t code_frame = 0;
    uintptr_t stack_frame = 0;
    bool code_mapped = false;
    bool stack_mapped = false;
    bool ok = vmm_address_space_create(&space);
    if (!ok) goto cleanup;

    code_frame = pmm_alloc_frame();
    stack_frame = pmm_alloc_frame();
    if (code_frame == 0 || stack_frame == 0) {
        ok = false;
        goto cleanup;
    }
    code_mapped = vmm_address_space_map_user(&space, USER_CODE_VIRT,
        code_frame, VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
    stack_mapped = vmm_address_space_map_user(&space, stack_page,
        stack_frame, VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
    if (!code_mapped || !stack_mapped ||
        !vmm_address_space_switch(&space)) {
        ok = false;
        goto cleanup;
    }

    static const uint8_t invalid_opcode[] = {0x0F, 0x0B};
    memcpy((void *)(uintptr_t)USER_CODE_VIRT, invalid_opcode,
           sizeof(invalid_opcode));
    memset((void *)(uintptr_t)stack_page, 0, PAGE_SIZE);
    bool code_protected = vmm_protect(USER_CODE_VIRT,
                                      VMM_PRESENT | VMM_USER);
    bool restored = vmm_address_space_switch(vmm_kernel_address_space());
    if (!code_protected || !restored) {
        ok = false;
        goto cleanup;
    }

    uint64_t result = 0;
    uint64_t fault_vector = 0;
    ok = usermode_run(&space, USER_CODE_VIRT, stack_top - 8,
                      &result, &fault_vector) && fault_vector == 6;
    if (fault_vector_out != NULL) *fault_vector_out = fault_vector;

cleanup:
    if ((read_cr3() & 0x000FFFFFFFFFF000ULL) !=
        (vmm_kernel_address_space()->root_phys & 0x000FFFFFFFFFF000ULL)) {
        vmm_address_space_switch(vmm_kernel_address_space());
    }
    if (code_mapped) vmm_address_space_unmap_user(&space, USER_CODE_VIRT);
    if (stack_mapped) vmm_address_space_unmap_user(&space, stack_page);
    if (space.root_phys != 0) vmm_address_space_destroy(&space);
    if (code_frame != 0) pmm_free_frame(code_frame);
    if (stack_frame != 0) pmm_free_frame(stack_frame);
    return ok;
}

bool usermode_driver_capability_test(void) {
    IRQ_GUARD;
    vmm_address_space_t space = {0};
    uintptr_t code_frame = 0;
    uintptr_t stack_frame = 0;
    bool code_mapped = false;
    bool stack_mapped = false;
    bool ok = vmm_address_space_create(&space);
    if (!ok) goto cleanup;

    code_frame = pmm_alloc_frame();
    stack_frame = pmm_alloc_frame();
    if (code_frame == 0 || stack_frame == 0) {
        ok = false;
        goto cleanup;
    }
    code_mapped = vmm_address_space_map_user(&space, USER_CODE_VIRT,
        code_frame, VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
    stack_mapped = vmm_address_space_map_user(&space, USER_STACK_VIRT,
        stack_frame, VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
    if (!code_mapped || !stack_mapped ||
        !vmm_address_space_switch(&space)) {
        ok = false;
        goto cleanup;
    }

    /* Probe the storage-capability syscall with no granted handles, then
     * attempt a sector read using a forged handle. Save both results on the
     * user stack so the kernel can verify the denial after normal user exit.
     */
    static const uint8_t program[] = {
        0xB8, HELIX_SYS_USB_GET_SERVICE_CHANNEL, 0x00, 0x00, 0x00,
        0x0F, 0x05,
        0x48, 0x89, 0x04, 0x24,
        0xB8, HELIX_SYS_USB_READ_SECTOR, 0x00, 0x00, 0x00,
        0xBF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x31, 0xF6,
        0xBA, 0x00, 0x10, 0x40, 0x00,
        0x0F, 0x05,
        0x48, 0x89, 0x44, 0x24, 0x08,
        0xB8, HELIX_SYS_EXIT, 0x00, 0x00, 0x00,
        0x31, 0xFF,
        0x0F, 0x05,
        0x0F, 0x0B,
    };
    memcpy((void *)(uintptr_t)USER_CODE_VIRT, program, sizeof(program));
    memset((void *)(uintptr_t)USER_STACK_VIRT, 0, PAGE_SIZE);
    bool protected = vmm_protect(USER_CODE_VIRT, VMM_PRESENT | VMM_USER);
    bool restored = vmm_address_space_switch(vmm_kernel_address_space());
    if (!protected || !restored) {
        ok = false;
        goto cleanup;
    }

    uint64_t exit_value = UINT64_MAX;
    ok = usermode_run(&space, USER_CODE_VIRT, USER_STACK_TOP - 16,
                      &exit_value, NULL) && exit_value == 0;
    if (ok && vmm_address_space_switch(&space)) {
        const volatile uint64_t *results =
            (const volatile uint64_t *)(uintptr_t)(USER_STACK_TOP - 16);
        uint64_t capability_result = results[0];
        uint64_t read_result = results[1];
        restored = vmm_address_space_switch(vmm_kernel_address_space());
        ok = restored && capability_result == (uint64_t)-1 &&
             read_result == (uint64_t)-1;
    } else {
        ok = false;
    }

cleanup:
    if ((read_cr3() & 0x000FFFFFFFFFF000ULL) !=
        (vmm_kernel_address_space()->root_phys & 0x000FFFFFFFFFF000ULL))
        (void)vmm_address_space_switch(vmm_kernel_address_space());
    if (code_mapped) vmm_address_space_unmap_user(&space, USER_CODE_VIRT);
    if (stack_mapped) vmm_address_space_unmap_user(&space, USER_STACK_VIRT);
    if (space.root_phys != 0) vmm_address_space_destroy(&space);
    if (code_frame != 0) pmm_free_frame(code_frame);
    if (stack_frame != 0) pmm_free_frame(stack_frame);
    return ok;
}
