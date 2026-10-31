/* wifi.c — Helix transport adapter for Linux WiFi drivers.
 *
 * This file provides the minimal translation layer that allows Linux's
 * WiFi driver model to work with Helix's native object framework.
 *
 * It implements a stub for WiFi interfaces that adapts the Linux wiphy
 * model to Helix's driver framework. The actual driver logic is expected
 * to be compiled with this header, allowing drivers like iwlwifi,
 * rtlwifi, brcmfmac, etc. to be built as optional components.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "transport/wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -- WiFi interface state -------------------------------------------------- */

typedef struct {
    wifi_interface_t *wifi;
    uint32_t state;         /* IFF_UP, IFF_DOWN */
    bool associated;
} wifi_dev_t;

/* -- Transport adapter functions ------------------------------------------- */

/* Allocate a WiFi interface */
wifi_interface_t *transport_wifi_alloc(const char *name) {
    wifi_dev_t *dev = (wifi_dev_t *)malloc(sizeof(wifi_dev_t));
    if (!dev) return NULL;
    
    dev->wifi = (wifi_interface_t *)malloc(sizeof(wifi_interface_t));
    if (!dev->wifi) {
        free(dev);
        return NULL;
    }
    
    dev->wifi->wiphy_index = 0;
    dev->wifi->netdev_index = 0;
    strncpy(dev->wifi->name, name, sizeof(dev->wifi->name) - 1);
    dev->wifi->name[sizeof(dev->wifi->name) - 1] = '\0';
    dev->wifi->current_config.ssid_len = 0;
    dev->wifi->current_config.psk_len = 0;
    dev->wifi->associated = false;
    dev->wifi->enabled = false;
    
    dev->state = 0;
    dev->associated = false;
    
    return dev->wifi;
}

/* Register a WiFi interface with the transport context */
int transport_wifi_register(wifi_interface_t *wifi) {
    /* In a real implementation, this would register with the object manager */
    return 0;
}

/* Free a WiFi interface */
void transport_wifi_free(wifi_interface_t *wifi) {
    if (wifi) {
        free(wifi);
    }
    return;
}

/* Bring interface up */
int transport_wifi_up(wifi_interface_t *wifi) {
    if (!wifi) return -1;
    wifi->enabled = true;
    return 0;
}

/* Bring interface down */
int transport_wifi_down(wifi_interface_t *wifi) {
    if (!wifi) return -1;
    wifi->enabled = false;
    wifi->associated = false;
    return 0;
}

/* WiFi scanning */
int transport_wifi_scan(wifi_interface_t *wifi, wifi_scan_result_t *results,
                        uint32_t max_results) {
    if (!wifi || !results || max_results == 0) return -1;
    /* Simulate scanning - in actual implementation, this would call
     * cfg80211_scan and return results */
    return 0;
}

/* WiFi association */
int transport_wifi_associate(wifi_interface_t *wifi,
                             const wifi_config_t *config) {
    if (!wifi || !config) return -1;
    strncpy(wifi->current_config.ssid, config->ssid,
            sizeof(wifi->current_config.ssid) - 1);
    wifi->current_config.ssid[sizeof(wifi->current_config.ssid) - 1] = '\0';
    wifi->current_config.ssid_len = config->ssid_len;
    strncpy(wifi->current_config.psk, config->psk,
            sizeof(wifi->current_config.psk) - 1);
    wifi->current_config.psk[sizeof(wifi->current_config.psk) - 1] = '\0';
    wifi->current_config.psk_len = config->psk_len;
    wifi->associated = true;
    return 0;
}

/* WiFi disassociation */
int transport_wifi_disassociate(wifi_interface_t *wifi) {
    if (!wifi) return -1;
    wifi->associated = false;
    return 0;
}

#ifdef __cplusplus
}
#endif