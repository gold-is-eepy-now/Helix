/* mac_subsystem.c — Mac/Darwin subsystem initialization and registration.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include "../include/mac_subsystem.h"
#include "../include/mach_o.h"
#include "../include/darwin_syscall.h"
#include "../include/mach_port.h"
#include "../include/mac_fs.h"
#include "../include/plist.h"
#include "../include/bundle.h"
#include "../include/darwin_env.h"
#include "../include/mac_appkit_x11.h"
#include <string.h>

/* Synthetic Mach-O header generator for embedded apps */
static void populate_synthetic_macho(uint8_t *buf, size_t maxlen, const char *segname, uint64_t entry) {
    memset(buf, 0, maxlen);
    mach_header_64_t *hdr = (mach_header_64_t *)buf;
    hdr->magic = MH_MAGIC_64;
    hdr->cputype = CPU_TYPE_X86_64;
    hdr->cpusubtype = CPU_SUBTYPE_X86_64_ALL;
    hdr->filetype = MH_EXECUTE;
    hdr->ncmds = 2;
    hdr->sizeofcmds = sizeof(segment_command_64_t) + sizeof(entry_point_command_t);
    hdr->flags = MH_PIE | MH_NOUNDEFS;

    segment_command_64_t *seg = (segment_command_64_t *)(buf + sizeof(mach_header_64_t));
    seg->cmd = LC_SEGMENT_64;
    seg->cmdsize = sizeof(segment_command_64_t);
    strncpy(seg->segname, segname, 16);
    seg->vmaddr = 0x100000000ULL;
    seg->vmsize = 0x10000;
    seg->fileoff = 0;
    seg->filesize = 0x1000;
    seg->maxprot = 7;
    seg->initprot = 5;

    entry_point_command_t *ep = (entry_point_command_t *)(buf + sizeof(mach_header_64_t) + sizeof(segment_command_64_t));
    ep->cmd = LC_MAIN;
    ep->cmdsize = sizeof(entry_point_command_t);
    ep->entryoff = entry;
    ep->stacksize = 0x800000;
}

void mac_subsystem_init(void) {
    mac_fs_init();
    darwin_env_init();
    mach_port_subsystem_init();
    ns_application_init(":0.0");

    /* Populate Calculator.app */
    const char *calc_plist = 
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "    <key>CFBundleExecutable</key>\n"
        "    <string>Calculator</string>\n"
        "    <key>CFBundleIdentifier</key>\n"
        "    <string>com.apple.calculator</string>\n"
        "    <key>CFBundleName</key>\n"
        "    <string>Calculator</string>\n"
        "    <key>CFBundlePackageType</key>\n"
        "    <string>APPL</string>\n"
        "    <key>CFBundleVersion</key>\n"
        "    <string>1.0.0</string>\n"
        "</dict>\n"
        "</plist>\n";
    mac_fs_mkdir("/Applications/Calculator.app", 0755);
    mac_fs_mkdir("/Applications/Calculator.app/Contents", 0755);
    mac_fs_mkdir("/Applications/Calculator.app/Contents/MacOS", 0755);
    mac_fs_mkdir("/Applications/Calculator.app/Contents/Resources", 0755);
    mac_fs_write_file_data("/Applications/Calculator.app/Contents/Info.plist",
                          calc_plist, strlen(calc_plist));

    uint8_t calc_macho[512];
    populate_synthetic_macho(calc_macho, sizeof(calc_macho), "__TEXT", 0x1040);
    mac_fs_write_file_data("/Applications/Calculator.app/Contents/MacOS/Calculator",
                          calc_macho, sizeof(calc_macho));

    /* Populate TextEdit.app */
    const char *textedit_plist = 
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "    <key>CFBundleExecutable</key>\n"
        "    <string>TextEdit</string>\n"
        "    <key>CFBundleIdentifier</key>\n"
        "    <string>com.apple.TextEdit</string>\n"
        "    <key>CFBundleName</key>\n"
        "    <string>TextEdit</string>\n"
        "    <key>CFBundlePackageType</key>\n"
        "    <string>APPL</string>\n"
        "    <key>CFBundleVersion</key>\n"
        "    <string>1.0.0</string>\n"
        "</dict>\n"
        "</plist>\n";
    mac_fs_mkdir("/Applications/TextEdit.app", 0755);
    mac_fs_mkdir("/Applications/TextEdit.app/Contents", 0755);
    mac_fs_mkdir("/Applications/TextEdit.app/Contents/MacOS", 0755);
    mac_fs_mkdir("/Applications/TextEdit.app/Contents/Resources", 0755);
    mac_fs_write_file_data("/Applications/TextEdit.app/Contents/Info.plist",
                          textedit_plist, strlen(textedit_plist));

    uint8_t textedit_macho[512];
    populate_synthetic_macho(textedit_macho, sizeof(textedit_macho), "__TEXT", 0x2040);
    mac_fs_write_file_data("/Applications/TextEdit.app/Contents/MacOS/TextEdit",
                          textedit_macho, sizeof(textedit_macho));
}
