/* virtio_gpu.h — the virtio-gpu device (QEMU's -device virtio-gpu-pci). See docs/12-graphics.md.
 *
 * Step 1 (here): find the device and read the display list it offers. Later steps: create
 * 2D resources, attach memory, set the scanout, and flush rectangles to the screen. */
#ifndef HELIX_VIRTIO_GPU_H
#define HELIX_VIRTIO_GPU_H

#include <stdbool.h>
#include <stdint.h>

/* Probe and set up the device. Logs what it found. False when there is no device. */
bool virtio_gpu_init(void);

/* How many scanouts (display outputs) the device reported: 0 when none was found. */
uint32_t virtio_gpu_scanout_count(void);

/* The size and state of scanout `index`. False if the index is out of range. */
bool virtio_gpu_scanout(uint32_t index, uint32_t *width, uint32_t *height, bool *enabled);

#endif /* HELIX_VIRTIO_GPU_H */
