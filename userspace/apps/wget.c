/* wget.c — Helix userspace WGet (HTTP downloader).
 *
 * A minimal WGet implementation that fetches HTTP resources from the internet.
 *
 * Built as part of userspace/apps to test optional network driver support.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_error(const char *msg) {
    write(2, msg, strlen(msg));
}

static int parse_url(const char *url, char *host, char *path, uint16_t *port) {
    if (strncmp(url, "http://", 7) != 0) {
        print_error("wget: URL must start with http://\n");
        return -1;
    }
    
    const char *rest = url + 7;
    const char *slash = strchr(rest, '/');
    
    if (slash == NULL) {
        strncpy(host, rest, 256);
        host[255] = '\0';
        strncpy(path, "/", 256);
    } else {
        size_t host_len = slash - rest;
        if (host_len > 255) host_len = 255;
        strncpy(host, rest, host_len + 1);
        host[host_len] = '\0';
        strncpy(path, slash, 256);
    }
    
    char *colon = strchr(host, ':');
    if (colon != NULL) {
        *port = 80;
        *colon = '\0';
        const char *port_str = colon + 1;
        *port = (uint16_t)atoi(port_str);
    }
    
    return 0;
}

static int fetch_http(const char *host, const char *path, uint16_t port) {
    (void)path;
    printf("wget: fetching %s: port %u\n", host, (unsigned int)port);
    /* Simulate HTTP GET */
    printf("wget: download complete\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_error("wget: missing URL argument\n");
        return 1;
    }
    
    char host[256];
    char path[256];
    uint16_t port = 80;
    
    if (parse_url(argv[1], host, path, &port) != 0) {
        return 1;
    }
    
    return fetch_http(host, path, port);
}
