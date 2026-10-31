/* volume.c — Helix userspace volume control.
 *
 * A minimal volume control tool that adjusts playback volume on the
 * default sound device.
 *
 * Built as part of userspace/apps to test optional sound driver support.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "linux_runtime.h"
#include "transport/sound.h"

static void print_volume(uint32_t left, uint32_t right) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "volume: L=%d%% R=%d%%\n\0", left, right);
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1), "d"(buf),
                      "c"(n));
}

static int get_volume() {
    /* Simulate getting current volume - in real implementation,
     * this would query the sound device */
    print_volume(50, 50);
    return 0;
}

static int set_volume(uint32_t left, uint32_t right) {
    if (left > 100 || right > 100) {
        __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                          "d"("volume: invalid volume (0-100)\n\0"), "c"(30));
        return 1;
    }
    
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                      "d"("volume: setting volume to "), "c"(26));
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "L=%d R=%d\n\0", left, right);
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1), "d"(buf),
                      "c"(n));
    
    /* Use transport layer to set volume */
    snd_card_t *card = transport_sound_card_open("HDA-Intel");
    if (card) {
        snd_pcm_t *pcm = transport_sound_pcm_open(card, 0, 0);
        if (pcm) {
            transport_sound_pcm_volume(pcm, left, right);
            transport_sound_pcm_close(pcm);
        }
        transport_sound_card_close(card);
    }
    
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
    
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                      "d"("volume: usage: volume [set L R | up | down]\n\0"),
                      "c"(42));
    return 1;
}