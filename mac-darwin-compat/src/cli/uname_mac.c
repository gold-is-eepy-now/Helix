/* uname_mac.c — Darwin uname tool.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

int mac_tool_uname(int argc, char **argv) {
    bool all = false;
    bool s = false;
    bool n = false;
    bool r = false;
    bool v = false;
    bool m = false;

    if (argc == 1) {
        s = true;
    } else {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-a") == 0) all = true;
            else if (strcmp(argv[i], "-s") == 0) s = true;
            else if (strcmp(argv[i], "-n") == 0) n = true;
            else if (strcmp(argv[i], "-r") == 0) r = true;
            else if (strcmp(argv[i], "-v") == 0) v = true;
            else if (strcmp(argv[i], "-m") == 0) m = true;
        }
    }

    if (all) {
        printf("Darwin Helix-Mac.local 23.5.0 Darwin Kernel Version 23.5.0: Helix Mac-Darwin Subsystem x86_64\n");
        return 0;
    }

    bool first = true;
    if (s) { printf("%sDarwin", first ? "" : " "); first = false; }
    if (n) { printf("%sHelix-Mac.local", first ? "" : " "); first = false; }
    if (r) { printf("%s23.5.0", first ? "" : " "); first = false; }
    if (v) { printf("%sDarwin Kernel Version 23.5.0", first ? "" : " "); first = false; }
    if (m) { printf("%sx86_64", first ? "" : " "); first = false; }
    printf("\n");

    return 0;
}
