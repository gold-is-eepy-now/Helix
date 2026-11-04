/* defaults.c — macOS user preferences management tool.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include <stdio.h>
#include <string.h>
#include "../../include/mac_fs.h"
#include "../../include/plist.h"

int mac_tool_defaults(int argc, char **argv) {
    if (argc < 2) {
        printf("defaults: [read | write | delete] [domain] [key] [value]\n");
        return 1;
    }

    const char *action = argv[1];

    if (strcmp(action, "read") == 0) {
        if (argc < 3) {
            printf("defaults read: missing domain\n");
            return 1;
        }
        const char *domain = argv[2];
        const char *key = (argc >= 4) ? argv[3] : NULL;

        char path[512];
        snprintf(path, sizeof(path), "/Users/helix/Library/Preferences/%s.plist", domain);

        char buf[4096];
        size_t len = 0;
        int rc = mac_fs_read_file_data(path, buf, sizeof(buf) - 1, &len);
        if (rc < 0 || len == 0) {
            printf("defaults: domain %s not found\n", domain);
            return 1;
        }
        buf[len] = '\0';

        plist_node_t *dom = plist_parse_xml(buf, len);
        if (!dom) {
            printf("defaults: error parsing plist for domain %s\n", domain);
            return 1;
        }

        if (key) {
            const char *val = plist_dict_get_string(dom, key);
            if (val) {
                printf("%s\n", val);
            } else {
                printf("defaults: key %s not found in domain %s\n", key, domain);
            }
        } else {
            printf("{\n");
            plist_node_t *child = dom->children;
            while (child) {
                printf("    \"%s\" = \"%s\";\n", child->key, child->val_str);
                child = child->next;
            }
            printf("}\n");
        }
        plist_free(dom);
        return 0;
    } else if (strcmp(action, "write") == 0) {
        if (argc < 5) {
            printf("defaults write: requires domain, key, and value\n");
            return 1;
        }
        const char *domain = argv[2];
        const char *key = argv[3];
        const char *val = argv[4];

        char path[512];
        snprintf(path, sizeof(path), "/Users/helix/Library/Preferences/%s.plist", domain);

        char buf[4096];
        size_t len = 0;
        plist_node_t *dom = NULL;
        if (mac_fs_read_file_data(path, buf, sizeof(buf) - 1, &len) >= 0 && len > 0) {
            buf[len] = '\0';
            dom = plist_parse_xml(buf, len);
        }
        if (!dom) {
            dom = plist_create_dict();
        }

        plist_dict_set_string(dom, key, val);

        char out_buf[4096];
        size_t out_len = plist_to_xml(dom, out_buf, sizeof(out_buf));
        mac_fs_write_file_data(path, out_buf, out_len);
        plist_free(dom);
        return 0;
    } else if (strcmp(action, "delete") == 0) {
        if (argc < 3) {
            printf("defaults delete: requires domain\n");
            return 1;
        }
        const char *domain = argv[2];
        char path[512];
        snprintf(path, sizeof(path), "/Users/helix/Library/Preferences/%s.plist", domain);
        /* Overwrite with empty dict */
        plist_node_t *empty = plist_create_dict();
        char out_buf[512];
        size_t out_len = plist_to_xml(empty, out_buf, sizeof(out_buf));
        mac_fs_write_file_data(path, out_buf, out_len);
        plist_free(empty);
        return 0;
    }

    printf("defaults: unknown command %s\n", action);
    return 1;
}
