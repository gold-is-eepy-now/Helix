/* linux/leds.h — LED class devices. Registration succeeds and records the device; Helix has
 * no sysfs, so nothing can set the brightness from outside yet. */
#ifndef HELIX_COMPAT_LINUX_LEDS_H
#define HELIX_COMPAT_LINUX_LEDS_H
#include <linux/device.h>
enum led_brightness { LED_OFF = 0, LED_ON = 1, LED_FULL = 255 };
#define LED_CORE_SUSPENDRESUME (1 << 16)
struct led_classdev {
    const char *name;
    enum led_brightness brightness;
    enum led_brightness max_brightness;
    unsigned int flags;
    const char *default_trigger;
    void (*brightness_set)(struct led_classdev *led_cdev, enum led_brightness brightness);
};
int devm_led_classdev_register(struct device *parent, struct led_classdev *led_cdev);
#endif
