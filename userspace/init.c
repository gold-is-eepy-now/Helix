/* init.c — /bin/init, the system init process (PID 1).
 *
 * The kernel starts this at boot after installing /bin. It does the bootstrap
 * work it can do today, then starts /bin/sh and waits for it. When the shell
 * exits, init exits too, and the kernel returns to its own shell. */

#include "libhelix.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    helix_write_fd(1, "\n");
    helix_write_fd(1, "========================================\n");
    helix_write_fd(1, "Helix OS - init (PID 1, /bin/init)\n");
    helix_write_fd(1, "========================================\n");
    helix_write_fd(1, "[init] starting system services...\n");
    helix_write_fd(1, "[init] (no services yet: drivers and daemons are later milestones)\n");

    helix_write_fd(1, "[init] starting /bin/sh\n");
    char *sh_argv[] = { "/bin/sh", NULL };
    int64_t status = helix_spawn("/bin/sh", sh_argv);
    if (status < 0) {
        helix_write_fd(1, "[init] could not start /bin/sh\n");
        return 1;
    }
    helix_write_fd(1, "[init] /bin/sh exited with status %d\n", (int)status);
    return 0;
}
