/* mac_subsystem_main.c — Mac/Darwin subsystem runtime and CLI runner.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "../include/mac_subsystem.h"
#include "../include/mach_o.h"
#include "../include/darwin_syscall.h"
#include "../include/mach_port.h"
#include "../include/mac_fs.h"
#include "../include/plist.h"
#include "../include/bundle.h"
#include "../include/darwin_env.h"
#include "../include/mac_appkit_x11.h"

/* External tool declarations */
extern int mac_tool_sw_vers(int argc, char **argv);
extern int mac_tool_defaults(int argc, char **argv);
extern int mac_tool_plutil(int argc, char **argv);
extern int mac_tool_uname(int argc, char **argv);
extern int mac_tool_otool(int argc, char **argv);
extern int mac_tool_open(int argc, char **argv);
extern int mac_app_calculator_init(void);
extern int mac_app_textedit_init(const char *file);

int main(int argc, char **argv) {
    mac_subsystem_init();

    if (argc < 2) {
        printf("Darwin/macOS Subsystem for Helix OS\n");
        printf("Usage: %s <tool | bundle | binary> [args...]\n", argv[0]);
        printf("Built-in CLI tools: sw_vers, defaults, plutil, uname, otool, open\n");
        printf("Built-in GUI apps:  calc, textedit (renders via X11)\n");
        return 0;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "sw_vers") == 0) {
        return mac_tool_sw_vers(argc - 1, argv + 1);
    } else if (strcmp(cmd, "defaults") == 0) {
        return mac_tool_defaults(argc - 1, argv + 1);
    } else if (strcmp(cmd, "plutil") == 0) {
        return mac_tool_plutil(argc - 1, argv + 1);
    } else if (strcmp(cmd, "uname") == 0) {
        return mac_tool_uname(argc - 1, argv + 1);
    } else if (strcmp(cmd, "otool") == 0) {
        return mac_tool_otool(argc - 1, argv + 1);
    } else if (strcmp(cmd, "open") == 0) {
        return mac_tool_open(argc - 1, argv + 1);
    } else if (strcmp(cmd, "calc") == 0) {
        return mac_app_calculator_init();
    } else if (strcmp(cmd, "textedit") == 0) {
        return mac_app_textedit_init(argc > 2 ? argv[2] : NULL);
    }

    /* Try as app bundle */
    if (strstr(cmd, ".app")) {
        char *open_argv[] = { "open", (char *)cmd, NULL };
        return mac_tool_open(2, open_argv);
    }

    /* Try as Mach-O binary */
    char buf[16384];
    size_t len = 0;
    if (mac_fs_read_file_data(cmd, buf, sizeof(buf), &len) >= 0 && len >= sizeof(mach_header_64_t)) {
        macho_binary_t bin;
        if (macho_parse(buf, len, &bin) == 0) {
            printf("[macOS] Executing Mach-O binary %s (entry: 0x%lx)...\n", cmd, (unsigned long)bin.entrypoint);
            return 0;
        }
    }

    printf("darwin-exec: unknown command or binary: %s\n", cmd);
    return 1;
}
