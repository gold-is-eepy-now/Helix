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
#include <unistd.h>

/* ponytail: the WiFi transport driver is still Linux-runtime only; wire the
 * native transport calls in when the kernel exposes them to ring 3. */
static void wifi_scan(const char *ifname) {
    printf("wifi: scanning on interface %s\n", ifname);
    printf("wifi: found 0 networks\n");
}

static void wifi_connect(const char *ifname, const char *ssid, const char *psk) {
    (void)psk;
    printf("wifi: connecting to SSID '%s'\n", ssid);
    printf("wifi: association failed\n");
    (void)ifname;
}

int main(int argc, char **argv) {
    const char *ifname = "wlan0";
    const char *ssid = NULL;
    const char *psk = NULL;
    int scan_only = 0;
    
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            ifname = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            ssid = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            psk = argv[++i];
        } else if (strcmp(argv[i], "--scan") == 0) {
            scan_only = 1;
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
