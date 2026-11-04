/* bundle.h — Mac Application Bundle (.app) inspection and launch.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#ifndef MAC_DARWIN_BUNDLE_H
#define MAC_DARWIN_BUNDLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "plist.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char          bundle_path[1024];
    char          executable_path[1024];
    char          identifier[128];
    char          name[128];
    char          version[32];
    char          package_type[8];
    plist_node_t *info_plist;
    bool          is_valid;
} mac_bundle_t;

int  bundle_open(const char *bundle_path, mac_bundle_t *out_bundle);
int  bundle_get_executable(const mac_bundle_t *bundle, char *buf, size_t maxlen);
int  bundle_launch(const mac_bundle_t *bundle, int argc, char **argv);
void bundle_close(mac_bundle_t *bundle);

#ifdef __cplusplus
}
#endif

#endif /* MAC_DARWIN_BUNDLE_H */
