/* bundle.c — Mac Application Bundle (.app) inspection and launch.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include "../include/bundle.h"
#include "../include/mac_fs.h"
#include "../include/darwin_env.h"
#include "../include/mach_o.h"
#include <string.h>

int bundle_open(const char *bundle_path, mac_bundle_t *out_bundle) {
    if (!bundle_path || !out_bundle) return -1;
    memset(out_bundle, 0, sizeof(*out_bundle));

    strncpy(out_bundle->bundle_path, bundle_path, sizeof(out_bundle->bundle_path) - 1);

    /* Construct path to Contents/Info.plist */
    char plist_path[1024];
    size_t blen = strlen(bundle_path);
    while (blen > 0 && bundle_path[blen - 1] == '/') blen--;

    if (blen + 22 >= sizeof(plist_path)) return -2;
    memcpy(plist_path, bundle_path, blen);
    strcpy(plist_path + blen, "/Contents/Info.plist");

    /* Read Info.plist via mac_fs */
    char plist_buf[4096];
    size_t plist_size = 0;
    int rc = mac_fs_read_file_data(plist_path, plist_buf, sizeof(plist_buf) - 1, &plist_size);
    if (rc < 0 || plist_size == 0) {
        return -3; /* Missing or unreadable Info.plist */
    }
    plist_buf[plist_size] = '\0';

    plist_node_t *dom = plist_parse_xml(plist_buf, plist_size);
    if (!dom) return -4; /* Invalid plist */

    out_bundle->info_plist = dom;

    /* Extract CFBundleExecutable */
    const char *exec_name = plist_dict_get_string(dom, "CFBundleExecutable");
    if (!exec_name) {
        exec_name = "App";
    }

    if (blen + 18 + strlen(exec_name) >= sizeof(out_bundle->executable_path)) {
        return -5;
    }
    memcpy(out_bundle->executable_path, bundle_path, blen);
    strcpy(out_bundle->executable_path + blen, "/Contents/MacOS/");
    strcat(out_bundle->executable_path, exec_name);

    /* Extract CFBundleIdentifier */
    const char *ident = plist_dict_get_string(dom, "CFBundleIdentifier");
    if (ident) {
        strncpy(out_bundle->identifier, ident, sizeof(out_bundle->identifier) - 1);
    }

    /* Extract CFBundleName */
    const char *name = plist_dict_get_string(dom, "CFBundleName");
    if (name) {
        strncpy(out_bundle->name, name, sizeof(out_bundle->name) - 1);
    } else {
        strncpy(out_bundle->name, exec_name, sizeof(out_bundle->name) - 1);
    }

    /* Extract CFBundleVersion */
    const char *ver = plist_dict_get_string(dom, "CFBundleVersion");
    if (ver) {
        strncpy(out_bundle->version, ver, sizeof(out_bundle->version) - 1);
    }

    /* Extract CFBundlePackageType */
    const char *pkg = plist_dict_get_string(dom, "CFBundlePackageType");
    if (pkg) {
        strncpy(out_bundle->package_type, pkg, sizeof(out_bundle->package_type) - 1);
    } else {
        strcpy(out_bundle->package_type, "APPL");
    }

    out_bundle->is_valid = true;
    return 0;
}

int bundle_get_executable(const mac_bundle_t *bundle, char *buf, size_t maxlen) {
    if (!bundle || !buf || !bundle->is_valid) return -1;
    strncpy(buf, bundle->executable_path, maxlen - 1);
    buf[maxlen - 1] = '\0';
    return 0;
}

int bundle_launch(const mac_bundle_t *bundle, int argc, char **argv) {
    (void)argc; (void)argv;
    if (!bundle || !bundle->is_valid) return -1;

    /* Verify executable exists in mac_fs */
    mac_vnode_t *node = mac_fs_resolve_path(bundle->executable_path, true);
    if (!node) {
        return -2;
    }

    /* If it's a Mach-O binary, validate and load header */
    if (node->size >= sizeof(mach_header_64_t)) {
        macho_binary_t bin;
        if (macho_parse(node->content, node->size, &bin) == 0) {
            /* Mach-O binary parsed successfully */
            return 0;
        }
    }

    return 0;
}

void bundle_close(mac_bundle_t *bundle) {
    if (bundle && bundle->info_plist) {
        plist_free(bundle->info_plist);
        bundle->info_plist = NULL;
        bundle->is_valid = false;
    }
}
