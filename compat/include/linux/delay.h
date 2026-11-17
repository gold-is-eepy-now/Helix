/* linux/delay.h — busy and sleeping delays, over Helix's TSC/PIT clock. */
#ifndef HELIX_COMPAT_LINUX_DELAY_H
#define HELIX_COMPAT_LINUX_DELAY_H
void compat_delay_us(unsigned long us);
void compat_delay_ms(unsigned long ms);
#define udelay(us) compat_delay_us(us)
#define msleep(ms) compat_delay_ms(ms)
#define usleep_range(min, max) compat_delay_us(min)
#endif
