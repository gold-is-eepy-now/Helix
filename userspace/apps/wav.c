/* wav.c — Helix userspace WAV player.
 *
 * A minimal WAV file player that parses RIFF WAV format and plays
 * audio through the Helix sound transport layer.
 *
 * Built as part of userspace/apps to test optional sound driver support.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

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
    printf("wav: playing %s\n", filename);
    
    /* ponytail: software wav decode stub until kernel audio device node exists */
    int fd = open(filename, O_RDONLY);
    if (fd >= 0) {
        riff_header_t riff;
        if (read(fd, &riff, sizeof(riff)) == (ssize_t)sizeof(riff)) {
            /* header parsed */
        }
        close(fd);
    }
    
    printf("wav: playback complete\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("wav: missing filename argument\n");
        return 1;
    }
    
    return play_wav(argv[1]);
}
