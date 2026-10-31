/* wifi.c — Helix userspace WiFi configuration tool.
 *
 * A minimal WiFi configuration tool that scans for networks, associates
 * with an SSID, and configures WPA-PSK.
 *
 * Built as part of userspace/apps to test optional WiFi driver support.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "linux_runtime.h"
#include "../../runtime/optional_drivers/transport/wifi.h"

static void wifi_scan(const char *ifname) {
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                      "d"("wifi: scanning on interface "),
                      "c"(25));
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                      "d"(ifname), "c"(strlen(ifname)));
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                      "d"("\n\0"), "c"(1));
    
    wifi_interface_t *wifi = transport_wifi_alloc(ifname);
    if (!wifi) {
        __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                          "d"("wifi: failed to allocate interface\n\0"),
                          "c"(32));
        return;
    }
    
    wifi_scan_result_t results[8];
    int count = transport_wifi_scan(wifi, results, 8);
    
    char buf[64];
    int n = 0;
    n += snprintf(&buf[n], sizeof(buf) - n, "wifi: found %d networks\n\0", count);
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1), "d"(buf),
                      "c"(n));
    
    transport_wifi_free(wifi);
}

static void wifi_connect(const char *ifname, const char *ssid, const char *psk) {
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                      "d"("wifi: connecting to SSID '"), "c"(32));
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1), "d"(ssid),
                      "c"(strlen(ssid)));
    __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                      "d"("'\n\0"), "c"(2));
    
    wifi_interface_t *wifi = transport_wifi_alloc(ifname);
    if (!wifi) {
        __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                          "d"("wifi: failed to allocate interface\n\0"),
                          "c"(32));
        return;
    }
    
    transport_wifi_up(wifi);
    
    wifi_config_t config;
    strncpy(config.ssid, ssid, sizeof(config.ssid) - 1);
    config.ssid[sizeof(config.ssid) - 1] = '\0';
    config.ssid_len = strlen(ssid);
    strncpy(config.psk, psk ? psk : "", sizeof(config.psk) - 1);
    config.psk[sizeof(config.psk) - 1] = '\0';
    config.psk_len = psk ? strlen(psk) : 0;
    
    int ret = transport_wifi_associate(wifi, &config);
    if (ret == 0) {
        __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                          "d"("wifi: associated successfully\n\0"), "c"(31));
    } else {
        __asm__ volatile ("int $0x80" : : "a"(SYSCALL_WRITE), "b"(1),
                          "d"("wifi: association failed\n\0"), "c"(25));
    }
    
    transport_wifi_free(wifi);
}

int main(int argc, char **argv) {
    const char *ifname = "wlan0";
    const char *ssid = NULL;
    const char *psk = NULL;
    bool scan_only = false;
    
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            ifname = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            ssid = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            psk = argv[++i];
        } else if (strcmp(argv[i], "--scan") == 0) {
            scan_only = true;
        }
    }
    
    if (scan_only || ssid == NULL) {
        wifi_scan(ifname);
    }
    
    if (ssid != NULL) {
        wifi_connect(ifname, ssid, psk);
    }
    
    return 0;
}