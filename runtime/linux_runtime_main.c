/* linux_runtime_main.c — Main entry point for the Linux/BSD runtime subsystem.
 *
 * This file implements a minimal runtime that can be launched as a
 * userspace process. It provides ELF loading and syscall translation
 * for both Linux and BSD compatibility.
 *
 * The runtime is designed to be launched as a regular Helix process and
 * will be invoked by the shell or as the init process.
 *
 * NOTE: This code is freestanding (no host libc). Any console output should
 * use the kernel syscall interface; for now this is a stub.
 */

#include "linux_runtime.h"
#include "linux_syscall.h"
#include "bsd_syscall.h"
#include "elf_loader.h"
#include <stddef.h>
#include <stdint.h>

/* Global state for the runtime */
static void *runtime_process_context = NULL;
static int runtime_fd = -1;  /* File descriptor for the current process */

/* ABI detection: determine if the loaded ELF binary is Linux or BSD based on
 * the ELF identification bytes. Currently Linux x86_64 is identified by the
 * OSABI byte at e_ident[7] being 0 (System V) or 3 (GNU/Linux). BSD uses
 * OSABI values 9 (FreeBSD), 12 (OpenBSD), 15 (NetBSD).
 *
 * Returns 1 for Linux, 0 for BSD, -1 if unknown.
 */
static int detect_abi(const uint8_t *e_ident) {
    if (e_ident == NULL) return -1;
    switch (e_ident[7]) {
        case 0x00:  /* System V ABI - assume Linux by default */
        case 0x03:  /* GNU/Linux */
            return 1;  /* Linux */
        case 0x09:  /* FreeBSD */
        case 0x0C:  /* OpenBSD */
        case 0x0F:  /* NetBSD */
            return 0;  /* BSD */
        default:
            return -1;  /* Unknown */
    }
}

/* Main entry point for the runtime */
void linux_runtime_start(void) {
    /* Initialize the FD-to-handle table (syscall map is lazily initialized
     * by linux_translate_number on first use).
     */
    linux_fd_table_init();

    /* In a real implementation, this would:
     * 1. Parse command line arguments
     * 2. Load ELF binary from disk or stdin
     * 3. Detect ABI (Linux vs BSD) from ELF header
     * 4. Set up memory mappings and initial process state
     * 5. Start executing the binary
     */

    /* For now: trigger both syscall map inits with pure lookups
     * (no int $0x80 — this validates the tables build correctly).
     */
    (void)linux_translate_number(0);   /* Linux table */
    (void)bsd_map_to_native(3);        /* BSD table */

    /* Spin while waiting for events */
    while (1) {
        __asm__ volatile ("pause");
    }
}

/* Entry point for a runtime process */
int main(int argc, char **argv) {
    /* The runtime starts here when a Linux or BSD process is launched */
    (void)argc;
    (void)argv;
    linux_runtime_start();
    return 0;
}