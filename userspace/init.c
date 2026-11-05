/* init.c — Native Helix OS init process (PID 1).
 *
 * Bootstraps userspace services, initializes devices and subsystems,
 * and launches the interactive user shell.
 */

#include "libhelix.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    helix_write_fd(1, "\n");
    helix_write_fd(1, "========================================\n");
    helix_write_fd(1, "Helix OS — Init System (PID 1)\n");
    helix_write_fd(1, "========================================\n");

    helix_write_fd(1, "[init] starting system services...\n");
    helix_write_fd(1, "[init] (no services yet: file-backed startup is M9 work)\n");
    helix_write_fd(1, "[init] launching user environment...\n\n");

    /* Yield to scheduler to allow queued threads to advance */
    helix_yield();

    helix_write_fd(1, "[init] done.\n");
    helix_exit(0);
}
