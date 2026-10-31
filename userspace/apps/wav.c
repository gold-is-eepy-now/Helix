/* wav.c — Helix userspace WAV player.
 *
 * A minimal WAV file player that parses RIFF WAV format and plays
 * audio through the Helix sound transport layer.
 *
 * Built as part of userspace/apps to test optional sound driver support.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "linux_runtime.h"
#include "transport/sound.h"

#pragma pack(push, 1)
typedef struct {
    uint32_t chunk_id;      /* "RIFF" */
    uint32_t chunk_size;    /* file size - 8 */
    uint32_t format;        /* "WAVE" */
} riff_header_t;

typedef struct {
    uint32_t chunk_id;      /* "fmt " */
    uint32_t chunk_size;    /* 16 for PCM */
    uint16_t audio_format;  /* 1 = PCM */
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} fmt_chunk_t;

typedef struct {
    uint32_t chunk_id;      /* "data" */
    uint32_t chunk_size;    /* size of audio data */
} data_chunk_t;
#pragma pack(pop)

static int play_wav(const char *filename) {
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                      "d"("wav: playing "), "c"(13));
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                      "d"(filename), "c"(strlen(filename)));
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                      "d"("\n\0"), "c"(1));
    
    /* Use the transport layer to play WAV */
    int ret = transport_sound_wav_play(filename, 50);
    
    if (ret == 0) {
        __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                          "d"("wav: playback complete\n\0"), "c"(23));
    } else {
        __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                          "d"("wav: playback failed\n\0"), "c"(21));
    }
    
    return ret;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                          "d"("wav: missing filename argument\n\0"), "c"(31));
        return 1;
    }
    
    return play_wav(argv[1]);
}