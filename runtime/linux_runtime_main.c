/* linux_runtime_main.c — Main entry point for the Linux/BSD runtime subsystem.
 *
 * This file implements a minimal Linux runtime that can be launched as a
 * userspace process. It provides basic ELF loading and syscall translation
 * for basic compatibility.
 *
 * The runtime is designed to be launched as a regular Helix process and
 * will be invoked by the shell or as the init process.
 *
 * NOTE: This code is freestanding (no host libc). Any console output should
 * use the kernel syscall interface; for now this is a stub.
 */

#include "linux_runtime.h"
#include "elf_loader.h"
#include "linux_syscall.h"
#include <stddef.h>
#include <stdint.h>

/* Global state for the runtime */
static void *runtime_process_context = NULL;
static int linux_process_fd = -1;  /* Linux file descriptor for the current process */

/* Main entry point for the Linux runtime */
void linux_runtime_start(void) {
    /* Initialize the FD-to-handle table (syscall map is lazily initialized
     * by linux_translate_number on first use).
     */
    linux_fd_table_init();

    /* In a real implementation, this would:
     * 1. Parse command line arguments
     * 2. Load ELF binary from disk or stdin
     * 3. Set up memory mappings and initial process state
     * 4. Start executing the Linux binary
     */

    /* For now: trigger syscall map init by calling the translate function
     * with a dummy number (this validates the table builds correctly).
     */
    (void)linux_translate_number(0);

    /* Spin while waiting for events */
    while (1) {
        __asm__ volatile ("pause");
    }
}

/* Entry point for a Linux process */
int main(int argc, char **argv) {
    /* The Linux runtime starts here when a Linux process is launched */
    (void)argc;
    (void)argv;
    linux_runtime_start();
    return 0;
}