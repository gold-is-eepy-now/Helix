/* virtio_gpu.h — the virtio-gpu device (QEMU's -device virtio-gpu-pci). See docs/12-graphics.md.
 *
 * Step 1: find the device and read the display list. Step 2 (here): one 2D resource backed
 * by plain memory, shown on scanout 0. Step 3: copy a changed rectangle from that memory to
 * the host and flush it to the screen. */
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

/* Create a 2D resource the size of scanout 0 (B8G8R8X8, 4 bytes a pixel), back it with
 * one contiguous run of memory, and show it on scanout 0. False if any step fails. */
bool virtio_gpu_display_init(void);

/* The memory behind the display: pixels, row by row, pitch bytes apart. False before
 * virtio_gpu_display_init has succeeded. */
bool virtio_gpu_backing(void **pixels, uint32_t *width, uint32_t *height, uint32_t *pitch);

/* Copy the rectangle from the backing memory to the host and flush it to the screen.
 * False if the rectangle is outside the display or the device refuses the command. */
bool virtio_gpu_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

#endif /* HELIX_VIRTIO_GPU_H */
