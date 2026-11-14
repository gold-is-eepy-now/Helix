/* init.c — PID 1 for the Linux-on-Helix experiment.
 *
 * Linux runs this from the initramfs as /init. It prints a banner to the
 * serial console and then powers the machine off, so a headless QEMU run
 * finishes on its own. Built static so it needs no libraries in the initramfs.
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>

int main(void) {
    printf("[linux-on-helix] /init is running as PID %d\n", (int)getpid());
    printf("[linux-on-helix] this Linux kernel was started by Limine on the Helix boot path\n");
    fflush(stdout);
    sync();
    reboot(RB_POWER_OFF);
    /* Only reached if the power-off request failed. */
    printf("[linux-on-helix] power-off failed; halting\n");
    for (;;) pause();
    return 0;
}
