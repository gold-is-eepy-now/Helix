/* transport.h — Common transport header for optional Linux drivers.
 *
 * This header defines the adapter layer that translates Linux driver
 * interfaces (net_device, drm_device, snd_card, etc.) into Helix's
 * native driver framework (driver_t, device_t, object_t).
 *
 * The optional drivers are NOT part of the kernel build. They are
 * compiled into a static library (`optional_drivers.a`) that the
 * Linux/BSD runtime links against. The runtime exposes the driver
 * interfaces to userspace through the native object model.
 *
 * These are stubs/adapters: they define the minimum Linux kernel
 * ABI needed to compile driver source files. They do NOT implement
 * a full Linux kernel emulation layer.
 */

#ifndef HELIX_TRANSPORT_H
#define HELIX_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Common types -------------------------------------------------------- */

/* A Helix driver handle (matches kernel/src/driver.h) */
typedef uint32_t driver_t;

/* A Helix device handle */
typedef uint32_t device_t;

/* A Helix object handle */
typedef uint32_t object_t;

/* Network interface index */
typedef uint32_t ifindex_t;

/* Maximum MTU for Ethernet */
#define ETH_MTU 1500
#define ETH_ZLEN 60

/* -- Transport context --------------------------------------------------- */

/* The transport context holds the adapter state for all optional drivers.
 * It is initialized by the runtime when the first optional driver is
 * activated. */
typedef struct transport_ctx {
    object_t net_object;      /* Network interface object */
    object_t display_object;  /* Display surface object */
    object_t audio_object;    /* Audio output object */
    uint32_t initialized;
} transport_ctx_t;

/* Initialize the transport context. Called once at runtime startup. */
void transport_init(void);

/* Look up the transport context. Returns NULL if not initialized. */
transport_ctx_t *transport_get(void);

/* Register a network interface with the Helix object manager. */
int transport_register_net(const char *name, ifindex_t ifindex);

/* Register a display surface with the Helix object manager. */
int transport_register_display(const char *name, uint32_t width, uint32_t height);

/* Register an audio output with the Helix object manager. */
int transport_register_audio(const char *name, uint32_t sample_rate);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_TRANSPORT_H */