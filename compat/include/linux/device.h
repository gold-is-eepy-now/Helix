/* linux/device.h — struct device, dev_* logging, device-managed allocation and PM ops. */
#ifndef HELIX_COMPAT_LINUX_DEVICE_H
#define HELIX_COMPAT_LINUX_DEVICE_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/compat_common.h>
#include <linux/string.h>

struct acpi_device;
struct acpi_device_id;
struct dev_pm_ops;

struct device_driver {
    const char *name;
    const struct acpi_device_id *acpi_match_table;
    const struct dev_pm_ops *pm;
};

struct device {
    const char *name;
    void *driver_data;
    struct acpi_device *acpi_companion;   /* NULL: Helix has no ACPI namespace to attach */
};

struct dev_pm_ops {
    int (*suspend)(struct device *dev);
    int (*resume)(struct device *dev);
    int (*poweroff_late)(struct device *dev);
};

/* Expands to designated initialisers, like Linux's macro. */
#define SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn) \
    .suspend = suspend_fn, .resume = resume_fn,
#define pm_sleep_ptr(ptr) (ptr)

static inline const char *dev_name(const struct device *dev) { return dev && dev->name ? dev->name : "(null)"; }
static inline void dev_set_drvdata(struct device *dev, void *data) { dev->driver_data = data; }
static inline void *dev_get_drvdata(const struct device *dev) { return dev->driver_data; }

#define dev_err(dev, fmt, ...)  kprintf("[compat] %s: error: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...) kprintf("[compat] %s: warning: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_info(dev, fmt, ...) kprintf("[compat] %s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...)  do { (void)(dev); } while (0)
#define dev_warn_ratelimited dev_warn

/* Device-managed allocation: Linux frees these when the device is unbound. Helix has no
 * unbind path for compat drivers yet, so they are plain allocations that stay until reboot. */
static inline void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp) { (void)dev; return kmalloc(size, gfp); }
static inline void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp) { (void)dev; return kzalloc(size, gfp); }
static inline void *devm_kmalloc_array(struct device *dev, size_t n, size_t size, gfp_t gfp) {
    (void)dev; return kcalloc(n, size, gfp);
}
#define devm_kzalloc_obj(dev, obj) devm_kzalloc(dev, sizeof(obj), GFP_KERNEL)

static inline void device_wakeup_enable(struct device *dev) { (void)dev; }
static inline void device_wakeup_disable(struct device *dev) { (void)dev; }
static inline int device_init_wakeup(struct device *dev, bool enable) { (void)dev; (void)enable; return 0; }

#define MODULE_DEVICE_TABLE(type, name)
#endif
