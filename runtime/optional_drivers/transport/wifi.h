/* transport/wifi.h — Helix transport adapter for Linux WiFi drivers.
 *
 * This header defines the Linux WiFi driver types (struct wiphy,
 * struct cfg80211_ops, etc.) in a Helix-compatible way. It's intentionally
 * minimal: real WiFi drivers (iwlwifi, rtlwifi, brcmfmac, ath11k) need
 * significant Linux kernel infrastructure, so this adapter provides just
 * enough to compile them against Helix's native stack.
 */

#ifndef HELIX_TRANSPORT_WIFI_H
#define HELIX_TRANSPORT_WIFI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- WiFi configuration types --------------------------------------------- */

#define WIFI_SSID_MAX 32
#define WIFI_PSK_MAX 64

typedef struct {
    char ssid[WIFI_SSID_MAX];
    uint32_t ssid_len;
    char psk[WIFI_PSK_MAX];
    uint32_t psk_len;
} wifi_config_t;

typedef struct {
    uint32_t frequency;   /* MHz */
    uint32_t channel;
    int32_t rssi;         /* dBm */
    char bssid[18];       /* XX:XX:XX:XX:XX:XX */
} wifi_scan_result_t;

/* -- WiFi interface handle ------------------------------------------------ */

typedef struct {
    uint32_t wiphy_index;
    uint32_t netdev_index;
    char name[16];
    wifi_config_t current_config;
    bool associated;
    bool enabled;
} wifi_interface_t;

/* -- Transport adapter functions ------------------------------------------ */

/* Allocate a WiFi interface */
wifi_interface_t *transport_wifi_alloc(const char *name);

/* Register a WiFi interface with the transport context */
int transport_wifi_register(wifi_interface_t *wifi);

/* Free a WiFi interface */
void transport_wifi_free(wifi_interface_t *wifi);

/* Bring interface up/down */
int transport_wifi_up(wifi_interface_t *wifi);
int transport_wifi_down(wifi_interface_t *wifi);

/* WiFi scanning and association */
int transport_wifi_scan(wifi_interface_t *wifi, wifi_scan_result_t *results, uint32_t max_results);
int transport_wifi_associate(wifi_interface_t *wifi, const wifi_config_t *config);
int transport_wifi_disassociate(wifi_interface_t *wifi);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_TRANSPORT_WIFI_H */