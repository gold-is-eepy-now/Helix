/* open_mac.c — macOS open tool for files and .app bundles.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../../include/bundle.h"
#include "../../include/mac_fs.h"

int mac_tool_open(int argc, char **argv) {
    if (argc < 2) {
        printf("open: [-a app] file_or_bundle\n");
        return 1;
    }

    const char *target = NULL;
    const char *app_name = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            app_name = argv[++i];
        } else if (argv[i][0] != '-') {
            target = argv[i];
        }
    }

    if (!target && !app_name) {
        printf("open: no target specified\n");
        return 1;
    }

    char bundle_path[1024];
    if (app_name) {
        if (app_name[0] == '/') {
            strncpy(bundle_path, app_name, sizeof(bundle_path) - 1);
        } else {
            snprintf(bundle_path, sizeof(bundle_path), "/Applications/%s%s",
                     app_name, (strstr(app_name, ".app") ? "" : ".app"));
        }
    } else {
        if (strstr(target, ".app")) {
            strncpy(bundle_path, target, sizeof(bundle_path) - 1);
        } else {
            /* Text or document file: open in TextEdit.app */
            strncpy(bundle_path, "/Applications/TextEdit.app", sizeof(bundle_path) - 1);
        }
    }

    mac_bundle_t bundle;
    int rc = bundle_open(bundle_path, &bundle);
    if (rc != 0) {
        printf("open: unable to open bundle %s (error %d)\n", bundle_path, rc);
        return 1;
    }

    printf("[macOS] Launching %s (%s)...\n", bundle.name, bundle.identifier);
    rc = bundle_launch(&bundle, 0, NULL);
    bundle_close(&bundle);

    return rc == 0 ? 0 : 1;
}
