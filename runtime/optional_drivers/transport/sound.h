/* transport/sound.h — Helix transport adapter for Linux sound drivers.
 *
 * Provides the minimal Linux sound subsystem types: struct snd_card,
 * struct snd_pcm, struct snd_hda_codec, etc. It maps Linux audio
 * interfaces to Helix's native object framework, letting Intel HDA
 * and Realtek codec drivers be compiled as optional components.
 *
 * The adapter is lightweight and does not emulate a full ALSA stack.
 */

#ifndef HELIX_TRANSPORT_SOUND_H
#define HELIX_TRANSPORT_SOUND_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Audio formats --------------------------------------------------------- */

#define SND_PCM_RATE_48000 48000
#define SND_PCM_RATE_44100 44100
#define SND_PCM_RATE_96000 96000

typedef enum {
    SND_PCM_FORMAT_S16_LE,
    SND_PCM_FORMAT_U8,
    SND_PCM_FORMAT_S32_LE,
} snd_pcm_format_t;

typedef struct {
    uint32_t rate;             /* Sample rate */
    uint32_t channels;         /* Stereo = 2 */
    snd_pcm_format_t format;   /* Sample format */
    uint32_t period_size;      /* Buffer size (frames) */
    uint32_t buffer_size;      /* Total buffer (frames) */
} snd_pcm_hw_params_t;

/* -- Sound device types --------------------------------------------------- */

typedef struct {
    char name[32];              /* Sound card name (e.g. "HDA-Intel-DSP") */
    uint32_t num_devices;       /* Number of PCM devices */
    uint32_t mixer_channels;    /* Volume controls */
    uint32_t supported_formats; /* Bitmask of SND_PCM_FORMAT_* */
} snd_card_t;

/* -- Playback handle ------------------------------------------------------- */

typedef struct {
    snd_card_t *card;           /* Owning sound card */
    uint32_t device;            /* Device index */
    uint32_t stream;            /* PCM stream (playback/capture) */
    bool active;                /* PCM playback active */
    uint32_t volume_left;       /* 0-100 */
    uint32_t volume_right;      /* 0-100 */
    void *buffer;               /* Audio data buffer */
    uint32_t buffer_size;       /* Buffer size in bytes */
    uint32_t ptr;               /* Current read position */
} snd_pcm_t;

/* -- Transport adapter functions ------------------------------------------- */

/* Open a sound card */
snd_card_t *transport_sound_card_open(const char *name);

/* Register sound card with Helix object manager */
int transport_sound_card_register(snd_card_t *card);

/* Close a sound card */
void transport_sound_card_close(snd_card_t *card);

/* Create a PCM device */
snd_pcm_t *transport_sound_pcm_open(snd_card_t *card, uint32_t device, uint32_t stream);

/* Configure PCM with hardware parameters */
int transport_sound_pcm_hw_params(snd_pcm_t *pcm, const snd_pcm_hw_params_t *params);

/* Start audio playback */
int transport_sound_pcm_start(snd_pcm_t *pcm);

/* Stop audio playback */
int transport_sound_pcm_stop(snd_pcm_t *pcm);

/* Close PCM device */
int transport_sound_pcm_close(snd_pcm_t *pcm);

/* Set volume (0-100) */
int transport_sound_pcm_volume(snd_pcm_t *pcm, uint32_t left, uint32_t right);

/* Wave file playback API -------------------------------------------------- */
int transport_sound_wav_play(const char *filename, uint32_t volume);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_TRANSPORT_SOUND_H */