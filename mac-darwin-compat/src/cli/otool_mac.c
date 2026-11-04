/* otool_mac.c — Mach-O object file inspection tool.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../../include/mach_o.h"
#include "../../include/mac_fs.h"

int mac_tool_otool(int argc, char **argv) {
    if (argc < 2) {
        printf("otool: [-h | -l | -L] <binary>\n");
        return 1;
    }

    bool show_header = false;
    bool show_load_cmds = false;
    bool show_libs = false;
    const char *filepath = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) show_header = true;
        else if (strcmp(argv[i], "-l") == 0) show_load_cmds = true;
        else if (strcmp(argv[i], "-L") == 0) show_libs = true;
        else if (argv[i][0] != '-') filepath = argv[i];
    }

    if (!filepath) {
        printf("otool: no binary specified\n");
        return 1;
    }

    if (!show_header && !show_load_cmds && !show_libs) {
        show_header = true;
    }

    char buf[16384];
    size_t len = 0;
    int rc = mac_fs_read_file_data(filepath, buf, sizeof(buf), &len);
    if (rc < 0 || len < sizeof(mach_header_64_t)) {
        printf("otool: %s: is not an object file\n", filepath);
        return 1;
    }

    macho_binary_t bin;
    if (macho_parse(buf, len, &bin) != 0) {
        printf("otool: %s: invalid Mach-O 64-bit binary\n", filepath);
        return 1;
    }

    printf("%s:\n", filepath);

    if (show_header) {
        printf("Mach header\n");
        printf("      magic  cputype cpusubtype  caps    filetype ncmds sizeofcmds      flags\n");
        printf(" 0x%08x 0x%07x          %u  0x00          %u    %2u       %4u 0x%08x\n",
               bin.header.magic, bin.header.cputype, bin.header.cpusubtype & 0xff,
               bin.header.filetype, bin.header.ncmds, bin.header.sizeofcmds, bin.header.flags);
    }

    if (show_load_cmds) {
        for (uint32_t i = 0; i < bin.segment_count; i++) {
            const segment_command_64_t *cmd = &bin.segments[i].cmd;
            printf("Load command %u\n", i);
            printf("      cmd LC_SEGMENT_64\n");
            printf("  cmdsize %u\n", cmd->cmdsize);
            printf("  segname %s\n", cmd->segname);
            printf("   vmaddr 0x%016lx\n", (unsigned long)cmd->vmaddr);
            printf("   vmsize 0x%016lx\n", (unsigned long)cmd->vmsize);
            printf("  fileoff %lu\n", (unsigned long)cmd->fileoff);
            printf(" filesize %lu\n", (unsigned long)cmd->filesize);
            printf("   nsects %u\n", cmd->nsects);
        }
    }

    if (show_libs) {
        for (uint32_t i = 0; i < bin.dylib_count; i++) {
            printf("\t%s\n", bin.dylibs[i]);
        }
    }

    return 0;
}
