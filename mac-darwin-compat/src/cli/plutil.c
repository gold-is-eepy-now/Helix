/* plutil.c — macOS property list utility tool.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../../include/mac_fs.h"
#include "../../include/plist.h"

int mac_tool_plutil(int argc, char **argv) {
    if (argc < 2) {
        printf("plutil: [-lint | -p] file.plist\n");
        return 1;
    }

    bool print_mode = false;
    const char *filepath = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0) {
            print_mode = true;
        } else if (strcmp(argv[i], "-lint") == 0) {
            print_mode = false;
        } else if (argv[i][0] != '-') {
            filepath = argv[i];
        }
    }

    if (!filepath) {
        printf("plutil: no file specified\n");
        return 1;
    }

    char buf[4096];
    size_t len = 0;
    int rc = mac_fs_read_file_data(filepath, buf, sizeof(buf) - 1, &len);
    if (rc < 0 || len == 0) {
        printf("%s: file not found or empty\n", filepath);
        return 1;
    }
    buf[len] = '\0';

    plist_node_t *dom = plist_parse_xml(buf, len);
    if (!dom) {
        printf("%s: parse error (invalid plist)\n", filepath);
        return 1;
    }

    if (print_mode) {
        printf("{\n");
        plist_node_t *child = dom->children;
        while (child) {
            printf("  \"%s\" => \"%s\"\n", child->key, child->val_str);
            child = child->next;
        }
        printf("}\n");
    } else {
        printf("%s: OK\n", filepath);
    }

    plist_free(dom);
    return 0;
}
