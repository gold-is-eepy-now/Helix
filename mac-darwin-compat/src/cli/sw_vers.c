/* sw_vers.c — macOS system version tool.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../../include/mac_fs.h"
#include "../../include/plist.h"

int mac_tool_sw_vers(int argc, char **argv) {
    char plist_data[2048];
    size_t len = 0;
    int rc = mac_fs_read_file_data("/System/Library/CoreServices/SystemVersion.plist",
                                  plist_data, sizeof(plist_data) - 1, &len);
    if (rc < 0 || len == 0) {
        printf("sw_vers: unable to read SystemVersion.plist\n");
        return 1;
    }
    plist_data[len] = '\0';

    plist_node_t *dom = plist_parse_xml(plist_data, len);
    if (!dom) {
        printf("sw_vers: corrupt SystemVersion.plist\n");
        return 1;
    }

    const char *prod_name = plist_dict_get_string(dom, "ProductName");
    const char *prod_ver = plist_dict_get_string(dom, "ProductVersion");
    const char *build_ver = plist_dict_get_string(dom, "ProductBuildVersion");

    if (!prod_name) prod_name = "macOS";
    if (!prod_ver) prod_ver = "14.5";
    if (!build_ver) build_ver = "23F79";

    bool show_name = true;
    bool show_ver = true;
    bool show_build = true;

    if (argc > 1) {
        show_name = false;
        show_ver = false;
        show_build = false;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-productName") == 0) show_name = true;
            else if (strcmp(argv[i], "-productVersion") == 0) show_ver = true;
            else if (strcmp(argv[i], "-buildVersion") == 0) show_build = true;
        }
    }

    if (show_name)  printf("ProductName:\t\t%s\n", prod_name);
    if (show_ver)   printf("ProductVersion:\t\t%s\n", prod_ver);
    if (show_build) printf("BuildVersion:\t\t%s\n", build_ver);

    plist_free(dom);
    return 0;
}
