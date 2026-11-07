/* volume.c — Helix userspace volume control.
 *
 * A minimal volume control tool that adjusts playback volume on the
 * default sound device.
 *
 * Built as part of userspace/apps to test optional sound driver support.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_volume(uint32_t left, uint32_t right) {
    printf("volume: L=%d%% R=%d%%\n", left, right);
}

static int get_volume(void) {
    /* Simulate getting current volume - in real implementation,
     * this would query the sound device */
    print_volume(50, 50);
    return 0;
}

static int set_volume(uint32_t left, uint32_t right) {
    if (left > 100 || right > 100) {
        printf("volume: invalid volume (0-100)\n");
        return 1;
    }
    
    printf("volume: setting volume to L=%d R=%d\n", left, right);
    print_volume(left, right);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) {
        /* No arguments: show current volume */
        return get_volume();
    }
    
    if (strcmp(argv[1], "set") == 0 && argc >= 4) {
        uint32_t left = (uint32_t)atoi(argv[2]);
        uint32_t right = (uint32_t)atoi(argv[3]);
        return set_volume(left, right);
    }
    
    if (strcmp(argv[1], "up") == 0) {
        /* Increase volume by 5% */
        return set_volume(55, 55);
    }
    
    if (strcmp(argv[1], "down") == 0) {
        /* Decrease volume by 5% */
        return set_volume(45, 45);
    }
    
    printf("volume: usage: volume [set L R | up | down]\n");
    return 1;
}
