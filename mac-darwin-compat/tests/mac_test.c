/* mac_test.c — Comprehensive unit & integration test suite for
 * the Mac/Darwin compatibility subsystem.
 *
 * Verifies:
 * 1. Mach-O 64-bit validation, parsing, segments, entrypoint, symbol lookup
 * 2. Darwin BSD & Mach syscall/trap translation table
 * 3. Darwin sysctl queries (kern.ostype, hw.machine, etc.)
 * 4. Mach ports, tasks, rights, and IPC messaging
 * 5. Mac-like filesystem (directory hierarchy, symlinks, case-insensitivity)
 * 6. Extended attributes (xattrs) & macOS metadata
 * 7. Property List (plist) parser, DOM tree, and serializer
 * 8. App bundle (.app) inspection and resolution
 * 9. Advanced Mac CLI tools (sw_vers, defaults, plutil, uname, otool, open)
 * 10. Mac AppKit/Cocoa GUI & Helix X11 server bridge
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "../include/mach_o.h"
#include "../include/darwin_syscall.h"
#include "../include/mach_port.h"
#include "../include/mac_fs.h"
#include "../include/plist.h"
#include "../include/bundle.h"
#include "../include/darwin_env.h"
#include "../include/mac_appkit_x11.h"

/* External tool & app declarations */
extern void mac_subsystem_init(void);
extern int mac_tool_sw_vers(int argc, char **argv);
extern int mac_tool_defaults(int argc, char **argv);
extern int mac_tool_plutil(int argc, char **argv);
extern int mac_tool_uname(int argc, char **argv);
extern int mac_tool_otool(int argc, char **argv);
extern int mac_tool_open(int argc, char **argv);
extern int mac_app_calculator_init(void);
extern int mac_app_textedit_init(const char *file);

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char *msg) {
    g_checks++;
    if (cond) {
        printf("  PASS: %s\n", msg);
    } else {
        printf("  FAIL: %s\n", msg);
        g_failures++;
    }
}

/* 1. Test Mach-O Parser */
static void test_macho_parser(void) {
    printf("[test] 1. Mach-O 64-bit binary parsing & validation\n");

    /* Test invalid buffer rejection */
    check(!macho_validate(NULL, 0), "rejects NULL buffer");
    uint8_t garbage[32] = {0};
    check(!macho_validate(garbage, sizeof(garbage)), "rejects garbage buffer");

    /* Create valid 64-bit Mach-O binary in memory */
    uint8_t macho_buf[1024];
    memset(macho_buf, 0, sizeof(macho_buf));

    mach_header_64_t *hdr = (mach_header_64_t *)macho_buf;
    hdr->magic = MH_MAGIC_64;
    hdr->cputype = CPU_TYPE_X86_64;
    hdr->cpusubtype = CPU_SUBTYPE_X86_64_ALL;
    hdr->filetype = MH_EXECUTE;
    hdr->ncmds = 3;
    hdr->sizeofcmds = sizeof(segment_command_64_t) + sizeof(section_64_t) +
                      sizeof(entry_point_command_t) + sizeof(symtab_command_t);
    hdr->flags = MH_PIE | MH_NOUNDEFS;

    uint8_t *p = macho_buf + sizeof(mach_header_64_t);

    /* Segment __TEXT with section __text */
    segment_command_64_t *seg = (segment_command_64_t *)p;
    seg->cmd = LC_SEGMENT_64;
    seg->cmdsize = sizeof(segment_command_64_t) + sizeof(section_64_t);
    strncpy(seg->segname, "__TEXT", 16);
    seg->vmaddr = 0x100000000ULL;
    seg->vmsize = 0x4000;
    seg->fileoff = 0;
    seg->filesize = 0x1000;
    seg->maxprot = 7;
    seg->initprot = 5;
    seg->nsects = 1;

    section_64_t *sect = (section_64_t *)(p + sizeof(segment_command_64_t));
    strncpy(sect->sectname, "__text", 16);
    strncpy(sect->segname, "__TEXT", 16);
    sect->addr = 0x100001000ULL;
    sect->size = 0x200;
    sect->offset = 0x1000;

    p += seg->cmdsize;

    /* Entry Point Command LC_MAIN */
    entry_point_command_t *ep = (entry_point_command_t *)p;
    ep->cmd = LC_MAIN;
    ep->cmdsize = sizeof(entry_point_command_t);
    ep->entryoff = 0x1000; /* Relative to __TEXT */
    ep->stacksize = 0x800000;

    p += ep->cmdsize;

    /* Symbol Table Command LC_SYMTAB */
    symtab_command_t *sym = (symtab_command_t *)p;
    sym->cmd = LC_SYMTAB;
    sym->cmdsize = sizeof(symtab_command_t);
    sym->symoff = 512;
    sym->nsyms = 1;
    sym->stroff = 768;
    sym->strsize = 64;

    /* Put dummy symbol and string */
    nlist_64_t *sym_entry = (nlist_64_t *)(macho_buf + 512);
    sym_entry->n_strx = 1; /* Offset in strtab */
    sym_entry->n_type = 0x0f; /* N_SECT | N_EXT */
    sym_entry->n_sect = 1;
    sym_entry->n_value = 0x100001000ULL;

    char *strtab = (char *)(macho_buf + 768);
    strtab[0] = '\0';
    strcpy(strtab + 1, "_main");

    check(macho_validate(macho_buf, sizeof(macho_buf)), "validates valid 64-bit Mach-O header");

    macho_binary_t bin;
    int rc = macho_parse(macho_buf, sizeof(macho_buf), &bin);
    check(rc == 0, "macho_parse succeeds");
    check(bin.header.magic == MH_MAGIC_64, "magic is MH_MAGIC_64");
    check(bin.header.cputype == CPU_TYPE_X86_64, "cputype is CPU_TYPE_X86_64");
    check(bin.segment_count == 1, "segment_count is 1");

    const macho_segment_t *s = macho_find_segment(&bin, "__TEXT");
    check(s != NULL, "finds __TEXT segment");
    check(s && s->section_count == 1, "__TEXT has 1 section");

    const section_64_t *sec = macho_find_section(&bin, "__TEXT", "__text");
    check(sec != NULL, "finds __text section");

    uint64_t entry = 0;
    check(macho_get_entrypoint(&bin, &entry), "retrieves entrypoint");
    check(entry == 0x100001000ULL, "entrypoint correctly resolved with base address");

    uint64_t sym_val = 0;
    const char *sym_name = macho_lookup_symbol(&bin, 0, &sym_val);
    check(sym_name && strcmp(sym_name, "_main") == 0, "symbol lookup finds _main");
    check(sym_val == 0x100001000ULL, "symbol address matches expected value");
}

/* 2. Test Darwin Syscall & Trap Translation */
static void test_darwin_syscalls(void) {
    printf("[test] 2. Darwin BSD syscall and Mach trap translation\n");

    bool is_mach = false;

    /* Standard Darwin BSD syscalls (with 0x2000000 class) */
    check(darwin_translate_number(0x2000000 | DARWIN_SYS_read, &is_mach) == 20 && !is_mach,
          "Darwin read (3) translates to Helix 20");
    check(darwin_translate_number(0x2000000 | DARWIN_SYS_write, &is_mach) == 21 && !is_mach,
          "Darwin write (4) translates to Helix 21");
    check(darwin_translate_number(0x2000000 | DARWIN_SYS_open, &is_mach) == 11 && !is_mach,
          "Darwin open (5) translates to Helix 11");
    check(darwin_translate_number(0x2000000 | DARWIN_SYS_close, &is_mach) == 12 && !is_mach,
          "Darwin close (6) translates to Helix 12");
    check(darwin_translate_number(0x2000000 | DARWIN_SYS_mmap, &is_mach) == 4 && !is_mach,
          "Darwin mmap (197) translates to Helix 4");
    check(darwin_translate_number(0x2000000 | DARWIN_SYS_munmap, &is_mach) == 5 && !is_mach,
          "Darwin munmap (73) translates to Helix 5");
    check(darwin_translate_number(0x2000000 | DARWIN_SYS_exit, &is_mach) == 23 && !is_mach,
          "Darwin exit (1) translates to Helix 23");
    check(darwin_translate_number(0x2000000 | DARWIN_SYS_sched_yield, &is_mach) == 8 && !is_mach,
          "Darwin sched_yield (327) translates to Helix 8");
    check(darwin_translate_number(0x2000000 | DARWIN_SYS_execve, &is_mach) == 25 && !is_mach,
          "Darwin execve (59) translates to Helix 25");

    /* Bare BSD syscalls without class prefix */
    check(darwin_translate_number(DARWIN_SYS_read, &is_mach) == 20, "bare read translates to Helix 20");
    check(darwin_translate_number(DARWIN_SYS_write, &is_mach) == 21, "bare write translates to Helix 21");

    /* Negative Mach traps */
    check(darwin_translate_number((uint64_t)-26, &is_mach) == 26 && is_mach,
          "classic negative trap -26 is Mach reply_port");
    check(darwin_translate_number((uint64_t)-28, &is_mach) == 28 && is_mach,
          "classic negative trap -28 is Mach task_self");
    check(darwin_translate_number((uint64_t)-31, &is_mach) == 31 && is_mach,
          "classic negative trap -31 is Mach msg");

    /* Mach class prefixed traps (0x1000000) */
    check(darwin_translate_number(0x1000000 | MACH_TRAP_task_self, &is_mach) == 28 && is_mach,
          "Mach class trap 0x100001c is task_self");
}

/* 3. Test Darwin Sysctl Queries */
static void test_darwin_sysctl(void) {
    printf("[test] 3. Darwin sysctl queries (kern.*, hw.*)\n");

    char str_buf[128];
    size_t len = sizeof(str_buf);

    check(darwin_sysctlbyname("kern.ostype", str_buf, &len, NULL, 0) == 0, "query kern.ostype");
    check(strcmp(str_buf, "Darwin") == 0, "kern.ostype is 'Darwin'");

    len = sizeof(str_buf);
    check(darwin_sysctlbyname("kern.osrelease", str_buf, &len, NULL, 0) == 0, "query kern.osrelease");
    check(strcmp(str_buf, "23.5.0") == 0, "kern.osrelease is '23.5.0'");

    len = sizeof(str_buf);
    check(darwin_sysctlbyname("hw.machine", str_buf, &len, NULL, 0) == 0, "query hw.machine");
    check(strcmp(str_buf, "x86_64") == 0, "hw.machine is 'x86_64'");

    len = sizeof(str_buf);
    check(darwin_sysctlbyname("hw.model", str_buf, &len, NULL, 0) == 0, "query hw.model");
    check(strcmp(str_buf, "Macmini8,1") == 0, "hw.model is 'Macmini8,1'");

    uint32_t ncpu = 0;
    len = sizeof(ncpu);
    check(darwin_sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == 0, "query hw.ncpu");
    check(ncpu == 4, "hw.ncpu is 4");

    uint32_t psize = 0;
    len = sizeof(psize);
    check(darwin_sysctlbyname("hw.pagesize", &psize, &len, NULL, 0) == 0, "query hw.pagesize");
    check(psize == 4096, "hw.pagesize is 4096");
}

/* 4. Test Mach Ports & IPC */
static void test_mach_ports(void) {
    printf("[test] 4. Mach ports, tasks, rights, and IPC messaging\n");

    mach_port_subsystem_init();

    mach_port_t task = mach_task_self();
    check(task != MACH_PORT_NULL, "mach_task_self returns valid port");

    mach_port_t host = mach_host_self();
    check(host != MACH_PORT_NULL, "mach_host_self returns valid port");

    mach_port_t reply = mach_reply_port();
    check(reply != MACH_PORT_NULL, "mach_reply_port allocates new port");

    mach_port_t my_port = MACH_PORT_NULL;
    kern_return_t kr = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &my_port);
    check(kr == KERN_SUCCESS && my_port != MACH_PORT_NULL, "mach_port_allocate succeeds");

    kr = mach_port_insert_right(task, my_port, my_port, MACH_PORT_RIGHT_SEND);
    check(kr == KERN_SUCCESS, "mach_port_insert_right succeeds");

    /* Send and receive a Mach message */
    struct {
        mach_msg_header_t hdr;
        char body[32];
    } send_msg;
    memset(&send_msg, 0, sizeof(send_msg));
    send_msg.hdr.msgh_bits = 0;
    send_msg.hdr.msgh_size = sizeof(send_msg);
    send_msg.hdr.msgh_remote_port = my_port;
    send_msg.hdr.msgh_local_port = reply;
    send_msg.hdr.msgh_id = 42;
    strcpy(send_msg.body, "Hello from Mach IPC!");

    kr = mach_msg_trap_impl(&send_msg.hdr, MACH_SEND_MSG, sizeof(send_msg), 0, MACH_PORT_NULL, 0, 0);
    check(kr == MACH_MSG_SUCCESS, "mach_msg send succeeds");

    struct {
        mach_msg_header_t hdr;
        char body[32];
    } rcv_msg;
    memset(&rcv_msg, 0, sizeof(rcv_msg));

    kr = mach_msg_trap_impl(&rcv_msg.hdr, MACH_RCV_MSG, 0, sizeof(rcv_msg), my_port, 0, 0);
    check(kr == MACH_MSG_SUCCESS, "mach_msg receive succeeds");
    check(rcv_msg.hdr.msgh_id == 42, "received message ID matches");
    check(strcmp(rcv_msg.body, "Hello from Mach IPC!") == 0, "received message payload matches");

    kr = mach_port_deallocate(task, my_port);
    check(kr == KERN_SUCCESS, "mach_port_deallocate succeeds");
}

/* 5. Test Mac-like Filesystem (mac_fs) */
static void test_mac_filesystem(void) {
    printf("[test] 5. Mac-like filesystem (directory hierarchy, symlinks, case-insensitivity)\n");

    mac_fs_init();

    /* Standard macOS directory verification */
    check(mac_fs_resolve_path("/System/Library/Frameworks", true) != NULL,
          "has /System/Library/Frameworks");
    check(mac_fs_resolve_path("/System/Library/Frameworks/Foundation.framework", true) != NULL,
          "has Foundation.framework");
    check(mac_fs_resolve_path("/System/Library/Frameworks/AppKit.framework", true) != NULL,
          "has AppKit.framework");
    check(mac_fs_resolve_path("/Library/Application Support", true) != NULL,
          "has /Library/Application Support");
    check(mac_fs_resolve_path("/Applications", true) != NULL,
          "has /Applications");
    check(mac_fs_resolve_path("/Users/helix/Desktop", true) != NULL,
          "has /Users/helix/Desktop");
    check(mac_fs_resolve_path("/Volumes/Macintosh HD", true) != NULL,
          "has /Volumes/Macintosh HD");

    /* Canonical Darwin Symlinks */
    mac_vnode_t *etc_link = mac_fs_resolve_path("/etc", false);
    check(etc_link && etc_link->type == MAC_NODE_SYMLINK, "/etc is a symlink");
    mac_vnode_t *etc_target = mac_fs_resolve_path("/etc", true);
    check(etc_target && strcmp(etc_target->name, "etc") == 0, "/etc resolves to /private/etc");

    /* Case-insensitivity (HFS+/APFS behavior) */
    mac_vnode_t *n1 = mac_fs_resolve_path("/Applications", true);
    mac_vnode_t *n2 = mac_fs_resolve_path("/applications", true);
    mac_vnode_t *n3 = mac_fs_resolve_path("/APPLICATIONS", true);
    check(n1 != NULL && n1 == n2 && n1 == n3, "case-insensitive lookup: /Applications == /applications == /APPLICATIONS");

    mac_vnode_t *s1 = mac_fs_resolve_path("/System/Library/CoreServices", true);
    mac_vnode_t *s2 = mac_fs_resolve_path("/system/library/coreservices", true);
    check(s1 != NULL && s1 == s2, "nested case-insensitive lookup matches");

    /* File creation, write, read, and stat */
    const char *test_data = "Apple Computer, Inc. macOS Darwin";
    int wrc = mac_fs_write_file_data("/Users/helix/test.txt", test_data, strlen(test_data));
    check(wrc == (int)strlen(test_data), "mac_fs_write_file_data succeeds");

    char read_buf[64];
    size_t read_len = 0;
    int rrc = mac_fs_read_file_data("/users/helix/TEST.TXT", read_buf, sizeof(read_buf), &read_len);
    check(rrc > 0 && read_len == strlen(test_data), "reads file via case-insensitive path");
    read_buf[read_len] = '\0';
    check(strcmp(read_buf, test_data) == 0, "read content matches written content");

    mac_stat_t st;
    check(mac_fs_stat("/users/helix/test.txt", &st) == 0, "mac_fs_stat succeeds");
    check(st.st_size == strlen(test_data), "stat size matches");
    check(st.st_uid == 501, "stat uid is standard macOS user (501)");
}

/* 6. Test Extended Attributes (xattr) */
static void test_extended_attributes(void) {
    printf("[test] 6. Extended attributes (xattr) & macOS metadata\n");

    const char *path = "/Users/helix/test.txt";
    const char *quarantine = "0083;66f01234;Safari;com.apple.Safari";

    int rc = mac_fs_setxattr(path, "com.apple.quarantine", quarantine, strlen(quarantine), 0);
    check(rc == 0, "setxattr com.apple.quarantine succeeds");

    uint8_t finder_info[32] = { 'T', 'E', 'X', 'T', 't', 't', 'x', 't' };
    rc = mac_fs_setxattr(path, "com.apple.FinderInfo", finder_info, sizeof(finder_info), 0);
    check(rc == 0, "setxattr com.apple.FinderInfo succeeds");

    char qbuf[64];
    int qlen = mac_fs_getxattr(path, "com.apple.quarantine", qbuf, sizeof(qbuf) - 1);
    check(qlen == (int)strlen(quarantine), "getxattr returns correct attribute length");
    qbuf[qlen] = '\0';
    check(strcmp(qbuf, quarantine) == 0, "retrieved quarantine string matches");

    char listbuf[256];
    int listlen = mac_fs_listxattr(path, listbuf, sizeof(listbuf));
    check(listlen > 0, "listxattr returns attributes");

    rc = mac_fs_removexattr(path, "com.apple.quarantine");
    check(rc == 0, "removexattr succeeds");
    check(mac_fs_getxattr(path, "com.apple.quarantine", qbuf, sizeof(qbuf)) < 0, "removed attribute is gone");
}

/* 7. Test Property List Engine */
static void test_plist_engine(void) {
    printf("[test] 7. Apple Property List (.plist) parser, DOM tree, and serializer\n");

    const char *sample_plist = 
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "    <key>CFBundleIdentifier</key>\n"
        "    <string>com.apple.calculator</string>\n"
        "    <key>CFBundleVersion</key>\n"
        "    <string>1.0.0</string>\n"
        "    <key>LSMinimumSystemVersion</key>\n"
        "    <string>14.0</string>\n"
        "    <key>NSHighResolutionCapable</key>\n"
        "    <true/>\n"
        "    <key>UIDeviceFamily</key>\n"
        "    <integer>1</integer>\n"
        "</dict>\n"
        "</plist>\n";

    plist_node_t *dom = plist_parse_xml(sample_plist, strlen(sample_plist));
    check(dom != NULL, "plist_parse_xml succeeds");
    check(dom && dom->type == PLIST_TYPE_DICT, "root node is dictionary");

    const char *ident = plist_dict_get_string(dom, "CFBundleIdentifier");
    check(ident && strcmp(ident, "com.apple.calculator") == 0, "retrieves CFBundleIdentifier");

    const char *ver = plist_dict_get_string(dom, "CFBundleVersion");
    check(ver && strcmp(ver, "1.0.0") == 0, "retrieves CFBundleVersion");

    bool hires = plist_dict_get_bool(dom, "NSHighResolutionCapable", false);
    check(hires == true, "retrieves boolean NSHighResolutionCapable");

    int64_t dev_fam = plist_dict_get_int(dom, "UIDeviceFamily", 0);
    check(dev_fam == 1, "retrieves integer UIDeviceFamily");

    /* Mutation & serialization */
    plist_dict_set_string(dom, "CFBundleName", "MacCalculator");
    const char *name = plist_dict_get_string(dom, "CFBundleName");
    check(name && strcmp(name, "MacCalculator") == 0, "modifies plist dictionary");

    char out_xml[1024];
    size_t out_len = plist_to_xml(dom, out_xml, sizeof(out_xml));
    check(out_len > 0, "plist_to_xml serializes DOM tree");
    check(strstr(out_xml, "MacCalculator") != NULL, "serialized XML contains modified key/value");

    plist_free(dom);
}

/* 8. Test App Bundle Loader */
static void test_app_bundle(void) {
    printf("[test] 8. Mac Application Bundle (.app) inspection\n");

    mac_subsystem_init();

    mac_bundle_t calc_bundle;
    int rc = bundle_open("/Applications/Calculator.app", &calc_bundle);
    check(rc == 0, "bundle_open /Applications/Calculator.app succeeds");
    check(calc_bundle.is_valid, "bundle is valid");
    check(strcmp(calc_bundle.identifier, "com.apple.calculator") == 0, "CFBundleIdentifier is com.apple.calculator");
    check(strcmp(calc_bundle.name, "Calculator") == 0, "CFBundleName is Calculator");
    check(strcmp(calc_bundle.package_type, "APPL") == 0, "package type is APPL");

    char exec_path[1024];
    check(bundle_get_executable(&calc_bundle, exec_path, sizeof(exec_path)) == 0, "bundle_get_executable succeeds");
    check(strstr(exec_path, "/Contents/MacOS/Calculator") != NULL, "executable path resolves to Contents/MacOS/Calculator");

    check(bundle_launch(&calc_bundle, 0, NULL) == 0, "bundle_launch succeeds");
    bundle_close(&calc_bundle);

    /* Test TextEdit.app */
    mac_bundle_t text_bundle;
    rc = bundle_open("/applications/textedit.app", &text_bundle); /* Case-insensitive path */
    check(rc == 0, "bundle_open /applications/textedit.app (case-insensitive) succeeds");
    check(strcmp(text_bundle.identifier, "com.apple.TextEdit") == 0, "CFBundleIdentifier is com.apple.TextEdit");
    bundle_close(&text_bundle);
}

/* 9. Test Advanced Mac CLI Tools */
static void test_mac_cli_tools(void) {
    printf("[test] 9. Advanced Mac CLI tools (sw_vers, defaults, plutil, uname, otool, open)\n");

    /* sw_vers */
    char *sw_argv[] = { "sw_vers", NULL };
    check(mac_tool_sw_vers(1, sw_argv) == 0, "sw_vers runs cleanly");

    char *sw_arg_ver[] = { "sw_vers", "-productVersion", NULL };
    check(mac_tool_sw_vers(2, sw_arg_ver) == 0, "sw_vers -productVersion runs cleanly");

    /* defaults */
    char *def_write[] = { "defaults", "write", "com.apple.dock", "autohide", "true", NULL };
    check(mac_tool_defaults(5, def_write) == 0, "defaults write com.apple.dock autohide true");

    char *def_read[] = { "defaults", "read", "com.apple.dock", "autohide", NULL };
    check(mac_tool_defaults(4, def_read) == 0, "defaults read com.apple.dock autohide");

    /* plutil */
    char *pl_lint[] = { "plutil", "-lint", "/System/Library/CoreServices/SystemVersion.plist", NULL };
    check(mac_tool_plutil(3, pl_lint) == 0, "plutil -lint SystemVersion.plist");

    /* uname */
    char *un_all[] = { "uname", "-a", NULL };
    check(mac_tool_uname(2, un_all) == 0, "uname -a reports Darwin");

    /* otool */
    char *otool_h[] = { "otool", "-h", "/Applications/Calculator.app/Contents/MacOS/Calculator", NULL };
    check(mac_tool_otool(3, otool_h) == 0, "otool -h inspects Mach-O header");

    /* open */
    char *open_app[] = { "open", "-a", "Calculator", NULL };
    check(mac_tool_open(3, open_app) == 0, "open -a Calculator launches bundle");
}

/* 10. Test Mac GUI AppKit-to-X11 Bridge */
static void test_mac_gui_appkit_x11(void) {
    printf("[test] 10. Mac GUI Cocoa/AppKit to Helix X11 server bridge\n");

    check(ns_application_init(":0.0") == 0, "ns_application_init succeeds");

    /* Create Mac window */
    ns_window_t *win = ns_window_create(50, 50, 400, 300, "Mac App Window");
    check(win != NULL, "ns_window_create creates window");
    check(win->width == 400 && win->height == 300, "window geometry matches");
    check(strcmp(win->title, "Mac App Window") == 0, "window title matches");

    /* Verify genuine macOS traffic light button hits */
    /* Close (Red): x=18, y=15 relative to window (50+18=68, 50+15=65) */
    check(ns_window_hit_test_traffic_lights(win, 68, 65) == 1, "traffic light Close (Red) detected");

    /* Minimize (Yellow): x=38, y=15 (50+38=88, 50+15=65) */
    check(ns_window_hit_test_traffic_lights(win, 88, 65) == 2, "traffic light Minimize (Yellow) detected");

    /* Zoom (Green): x=58, y=15 (50+58=108, 50+15=65) */
    check(ns_window_hit_test_traffic_lights(win, 108, 65) == 3, "traffic light Zoom (Green) detected");

    /* Content area click should not trigger traffic lights */
    check(ns_window_hit_test_traffic_lights(win, 150, 150) == 0, "click in content area returns 0");

    /* Add Cocoa views */
    ns_view_t *lbl = ns_label_create(20, 40, 200, 30, "AppKit Label");
    check(lbl != NULL, "ns_label_create succeeds");
    ns_window_add_subview(win, lbl);

    ns_view_t *btn = ns_button_create(20, 80, 100, 32, "Click Me", NULL, NULL);
    check(btn != NULL, "ns_button_create succeeds");
    ns_window_add_subview(win, btn);

    check(win->view_count == 2, "window contains 2 subviews");

    /* Order front and test drawing / wire commands */
    ns_window_order_front(win);
    check(win->is_visible, "window is visible after order_front");

    /* Test X11 wire protocol packet generation */
    check(x11_bridge_send_create_window(0, win->x11_window_id, win->x, win->y, win->width, win->height) == 0,
          "X11 bridge send create window");
    check(x11_bridge_send_map_window(0, win->x11_window_id) == 0,
          "X11 bridge send map window");
    check(x11_bridge_send_fill_rect(0, win->x11_window_id, 0, 0, win->width, 28, MAC_COLOR_TITLEBAR) == 0,
          "X11 bridge send titlebar fill rectangle");

    /* Test event dispatch: Close button click closes window */
    ns_event_t close_ev;
    memset(&close_ev, 0, sizeof(close_ev));
    close_ev.type = NS_EVENT_LEFT_MOUSE_DOWN;
    close_ev.x = 68;
    close_ev.y = 65;
    ns_dispatch_event(&close_ev);
    check(!win->is_visible, "clicking Red traffic light closes window");

    /* Test Calculator GUI app initialization */
    check(mac_app_calculator_init() == 0, "mac_app_calculator_init succeeds");

    /* Test TextEdit GUI app initialization */
    check(mac_app_textedit_init("test.txt") == 0, "mac_app_textedit_init succeeds");
}

int main(void) {
    printf("====================================================\n");
    printf("Helix OS — Mac/Darwin Compatibility Subsystem Tests\n");
    printf("====================================================\n");

    test_macho_parser();
    test_darwin_syscalls();
    test_darwin_sysctl();
    test_mach_ports();
    test_mac_filesystem();
    test_extended_attributes();
    test_plist_engine();
    test_app_bundle();
    test_mac_cli_tools();
    test_mac_gui_appkit_x11();

    printf("====================================================\n");
    if (g_failures == 0) {
        printf("RESULT: PASS (%d checks passed, 0 failures)\n", g_checks);
        return 0;
    } else {
        printf("RESULT: FAIL (%d/%d checks failed)\n", g_failures, g_checks);
        return 1;
    }
}
