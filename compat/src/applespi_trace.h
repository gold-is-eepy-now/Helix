/* applespi_trace.h — stands in for the Linux tracepoints applespi.c uses. Helix has no
 * tracepoints; each call does nothing. The signatures match the upstream trace events. */
#ifndef HELIX_COMPAT_APPLESPI_TRACE_H
#define HELIX_COMPAT_APPLESPI_TRACE_H
#include <linux/types.h>
#include "applespi.h"
#define APPLESPI_TRACE_STUB(name) \
    static inline void trace_##name(enum applespi_evt_type evt, enum applespi_pkt_type pkt, \
                                    u8 *buf, size_t len) { (void)evt; (void)pkt; (void)buf; (void)len; }
APPLESPI_TRACE_STUB(applespi_tp_ini_cmd)
APPLESPI_TRACE_STUB(applespi_backlight_cmd)
APPLESPI_TRACE_STUB(applespi_caps_lock_cmd)
APPLESPI_TRACE_STUB(applespi_keyboard_data)
APPLESPI_TRACE_STUB(applespi_touchpad_data)
APPLESPI_TRACE_STUB(applespi_unknown_data)
APPLESPI_TRACE_STUB(applespi_bad_crc)
static inline void trace_applespi_irq_received(enum applespi_evt_type evt, enum applespi_pkt_type pkt) {
    (void)evt; (void)pkt;
}
#endif
