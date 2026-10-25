/* shell.c — first interactive user-space command loop. */
#include "helix.h"

static helix_size_t string_length(const char *s) {
    helix_size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}
static int string_equal(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) { a++; b++; }
    return *a == *b;
}
static void print(const char *s) { (void)helix_write(s, string_length(s)); }
static void print_line(const char *s) { print(s); print("\n"); }
static void print_hex_byte(unsigned char value) {
    static const char digits[] = "0123456789abcdef";
    char out[2] = { digits[value >> 4], digits[value & 15] };
    (void)helix_write(out, 2);
}

static int parse_i32(const char **cursor, long *value) {
    const char *p = *cursor;
    int negative = 0;
    unsigned long magnitude = 0;
    if (*p == '+' || *p == '-') { negative = *p == '-'; p++; }
    if (*p < '0' || *p > '9') return 0;
    do {
        unsigned int digit = (unsigned int)(*p - '0');
        unsigned long limit = negative ? 2147483648UL : 2147483647UL;
        if (magnitude > (limit - digit) / 10UL) return 0;
        magnitude = magnitude * 10UL + digit;
        p++;
    } while (*p >= '0' && *p <= '9');
    *value = negative ? -(long)magnitude : (long)magnitude;
    *cursor = p;
    return 1;
}

static void print_i32(long value) {
    char digits[12];
    unsigned int count = 0;
    unsigned long magnitude;
    if (value < 0) {
        print("-");
        magnitude = (unsigned long)(-(value + 1)) + 1UL;
    } else magnitude = (unsigned long)value;
    do { digits[count++] = (char)('0' + magnitude % 10UL); magnitude /= 10UL; }
    while (magnitude != 0 && count < sizeof(digits));
    while (count != 0) { char c = digits[--count]; (void)helix_write(&c, 1); }
}

static int run_calculator(const char *expression) {
    long left, right, result;
    char operation;
    const char *p = expression;
    if (!parse_i32(&p, &left)) return 0;
    while (*p == ' ') p++;
    operation = *p++;
    if (operation != '+' && operation != '-' && operation != '*' && operation != '/')
        return 0;
    while (*p == ' ') p++;
    if (!parse_i32(&p, &right)) return 0;
    while (*p == ' ') p++;
    if (*p != '\0') return 0;
    if (operation == '+') result = left + right;
    else if (operation == '-') result = left - right;
    else if (operation == '*') result = left * right;
    else {
        if (right == 0) { print_line("calc: division by zero"); return 1; }
        result = left / right;
    }
    if (result < -2147483648L || result > 2147483647L) {
        print_line("calc: result is outside the 32-bit integer range");
        return 1;
    }
    print_i32(result); print("\n");
    return 1;
}

static void run_installer(void) {
    char target[40];
    char answer[32];
    (void)helix_clear();
    print_line("========================================");
    print_line("          HELIX OS INSTALLER");
    print_line("========================================");
    print_line("Detected devices:");
    (void)helix_drivers();
    print_line("Layout: GPT, FAT32 EFI System Partition, and HFS data partition.");
    print_line("Installation overwrites the selected disk's partition layout and first 64 MiB.");
    print_line("Only continue if this is the disk you intend to erase.");
    print("Enter the exact target device name (e.g. sata0), or blank to cancel: ");
    long target_count = helix_read(target, sizeof(target) - 1);
    if (target_count <= 0) { print_line("\nInstaller input failed."); return; }
    while (target_count > 0 &&
           (target[target_count - 1] == '\n' || target[target_count - 1] == '\r'))
        target_count--;
    target[target_count] = '\0';
    if (target_count == 0) { print_line("\nInstallation cancelled."); return; }
    print("Type INSTALL to overwrite ");
    print(target);
    print(": ");
    long count = helix_read(answer, sizeof(answer) - 1);
    if (count <= 0) { print_line("\nInstaller input failed."); return; }
    if (answer[count - 1] == '\n') count--;
    answer[count] = '\0';
    if (!string_equal(answer, "INSTALL")) {
        print_line("\nInstallation cancelled.");
        return;
    }
    print_line("\nInstalling Helix with HFS...");
    if (helix_install(target) == 0) print_line("Installation complete.");
    else print_line("Installation failed; see the message above.");
}

void _start(void) {
    char line[128];
    long uart_capability = helix_uart_capability();
    for (;;) {
        print("user@helix:~$ ");
        long count = helix_read(line, sizeof(line) - 1);
        if (count <= 0) { print_line("input error"); continue; }
        if (line[count - 1] == '\n') count--;
        line[count] = '\0';
        if (string_equal(line, "help")) {
            print_line("apps: edit [HFS_PATH], calc A OP B, about (sysinfo)");
            print_line("commands: help, echo, version, sysinfo, neofetch, drivers");
            print_line("files: ls, cd DIR|..|/, pwd, usb, hfs ls, hfs cat NAME");
            print_line("USB: usb read LBA, usb ls [usb0|cluster], usb cat/put/rm NAME");
            print_line("other: hfs rm/format, installer, uart X, clear, exit");
        } else if (string_equal(line, "version")) {
            (void)helix_version();
        } else if (string_equal(line, "sysinfo") || string_equal(line, "neofetch")) {
            (void)helix_sysinfo();
        } else if (string_equal(line, "about")) {
            (void)helix_sysinfo();
        } else if (line[0] == 'c' && line[1] == 'a' && line[2] == 'l' &&
                   line[3] == 'c' && line[4] == ' ') {
            if (!run_calculator(line + 5))
                print_line("usage: calc <integer> <+|-|*|/> <integer>");
        } else if (string_equal(line, "edit")) {
            (void)helix_edit();
            (void)helix_clear();
            print_line("Helix Edit closed; scratch contents remain in RAM for this boot.");
        } else if (line[0] == 'e' && line[1] == 'd' && line[2] == 'i' &&
                   line[3] == 't' && line[4] == ' ' && line[5] != '\0') {
            if (helix_edit_file(line + 5) != 0)
                print_line("HFS file could not be loaded by Helix Edit.");
        } else if (string_equal(line, "drivers")) {
            (void)helix_drivers();
        } else if (string_equal(line, "ls")) {
            if (!helix_fat32_ls())
                print_line("ls failed; check that a FAT32 USB volume is available.");
        } else if (string_equal(line, "pwd")) {
            (void)helix_fat32_pwd();
        } else if (line[0] == 'c' && line[1] == 'd' && line[2] == ' ' &&
                   line[3] != '\0') {
            if (!helix_fat32_cd(line + 3))
                print_line("cd failed; use a short 8.3 directory name, '..', or '/'.");
        } else if (string_equal(line, "hfs ls")) {
            if (helix_hfs_list() != 0)
                print_line("HFS mount or directory validation failed.");
        } else if (line[0] == 'h' && line[1] == 'f' && line[2] == 's' &&
                   line[3] == ' ' && line[4] == 'f' && line[5] == 'o' &&
                   line[6] == 'r' && line[7] == 'm' && line[8] == 'a' &&
                   line[9] == 't' && line[10] == ' ' && line[11] != '\0') {
            print_line("This formats a whole blank disk as a raw HFS volume.");
            print_line("It refuses disks with nonzero data in LBAs 0, 1, or the last sector.");
            if (helix_hfs_format(line + 11) == 0)
                print_line("HFS format complete.");
            else
                print_line("HFS format failed; see the reason above.");
        } else if (line[0] == 'h' && line[1] == 'f' && line[2] == 's' &&
                   line[3] == ' ' && line[4] == 'r' && line[5] == 'm' &&
                   line[6] == ' ' && line[7] != '\0') {
            if (helix_hfs_delete(line + 7) != 0)
                print_line("HFS delete failed (check the path and volume).");
        } else if (string_equal(line, "installer")) {
            run_installer();
        } else if (line[0] == 'h' && line[1] == 'f' && line[2] == 's' &&
                   line[3] == ' ' && line[4] == 'c' && line[5] == 'a' &&
                   line[6] == 't' && line[7] == ' ' && line[8] != '\0') {
            if (helix_hfs_cat(line + 8) != 0)
                print_line("HFS file read failed (check the name and volume).");
        } else if (string_equal(line, "usb")) {
            (void)helix_usb_info();
        } else if (line[0] == 'u' && line[1] == 's' && line[2] == 'b' &&
                   line[3] == ' ' && line[4] == 'p' && line[5] == 'u' &&
                   line[6] == 't' && line[7] == ' ' && line[8] != '\0') {
            char *name = line + 8;
            char *content = name;
            while (*content != '\0' && *content != ' ') content++;
            if (*content == '\0') {
                print_line("usage: usb put NAME.TXT TEXT");
            } else {
                *content++ = '\0';
                helix_size_t length = 0;
                while (content[length] != '\0') length++;
                if (helix_fat32_write(name, content, length) != 0)
                    print_line("FAT32 write failed (check the short .TXT name and volume).");
                else
                    print_line("FAT32 file saved.");
            }
        } else if (line[0] == 'u' && line[1] == 's' && line[2] == 'b' &&
                   line[3] == ' ' && line[4] == 'r' && line[5] == 'm' &&
                   line[6] == ' ' && line[7] != '\0') {
            if (helix_fat32_delete(line + 7) != 0)
                print_line("FAT32 delete failed (check the short .TXT name and volume).");
            else
                print_line("FAT32 file removed.");
        } else if (line[0] == 'u' && line[1] == 's' && line[2] == 'b' &&
                   line[3] == ' ' && line[4] == 'l' && line[5] == 's' &&
                   (line[6] == '\0' || line[6] == ' ')) {
            if (string_equal(line + 7, "usb0")) {
                (void)helix_fat32_list_all();
                continue;
            }
            unsigned long cluster = 0;
            int valid = 1;
            if (line[6] == ' ') {
                valid = line[7] != '\0';
                for (helix_size_t i = 7; valid && line[i] != '\0'; i++) {
                    if (line[i] < '0' || line[i] > '9' ||
                        cluster > (0xffffffffUL -
                            (unsigned long)(line[i] - '0')) / 10UL) {
                        valid = 0;
                        break;
                    }
                    cluster = cluster * 10UL + (unsigned long)(line[i] - '0');
                }
            }
            if (!valid) print_line("usage: usb ls [directory-cluster]");
            else (void)helix_fat32_list((unsigned int)cluster);
        } else if (line[0] == 'u' && line[1] == 's' && line[2] == 'b' &&
                   line[3] == ' ' && line[4] == 'c' && line[5] == 'a' &&
                   line[6] == 't' && line[7] == ' ' && line[8] != '\0') {
            if (!helix_fat32_cat(line + 8))
                print_line("FAT32 file read failed; check the USB volume and path.");
        } else if (line[0] == 'u' && line[1] == 's' && line[2] == 'b' &&
                   line[3] == ' ' && line[4] == 'r' && line[5] == 'e' &&
                   line[6] == 'a' && line[7] == 'd' && line[8] == ' ') {
            unsigned long lba = 0;
            int valid = line[9] != '\0';
            for (helix_size_t i = 9; valid && line[i] != '\0'; i++) {
                if (line[i] < '0' || line[i] > '9' ||
                    lba > (0xffffffffUL - (unsigned long)(line[i] - '0')) / 10UL) {
                    valid = 0;
                    break;
                }
                lba = lba * 10UL + (unsigned long)(line[i] - '0');
            }
            unsigned char sector[512];
            long cap = helix_usb_service_channel();
            if (!valid) {
                print_line("usage: usb read <LBA>");
            } else if (cap < 0 || helix_usb_read_sector((unsigned int)cap,
                       lba, sector) != 0) {
                print_line("USB sector read failed (no read capability/device or LBA unavailable).");
            } else {
                print("sector ");
                /* decimal LBA output is intentionally omitted in this tiny shell */
                print("read; first 16 bytes: ");
                for (unsigned int i = 0; i < 16; i++) {
                    if (i != 0) print(" ");
                    print_hex_byte(sector[i]);
                }
                print("\n");
            }
        } else if (line[0] == 'u' && line[1] == 'a' && line[2] == 'r' &&
                   line[3] == 't' && line[4] == ' ' && line[5] != '\0' &&
                   line[6] == '\0') {
            if (uart_capability >= 0 && helix_uart_write_byte(
                    (unsigned int)uart_capability,
                    (unsigned char)line[5]) == 0)
                print_line("UART device control sent the byte.");
            else
                print_line("UART device control failed (try a different byte).");
        } else if (string_equal(line, "clear")) {
            (void)helix_clear();
        } else if (string_equal(line, "exit")) {
            helix_exit(0);
        } else if (line[0] == 'e' && line[1] == 'c' && line[2] == 'h' &&
                   line[3] == 'o' && (line[4] == ' ' || line[4] == '\0')) {
            print_line(line[4] == ' ' ? line + 5 : "");
        } else if (line[0] != '\0') {
            print_line("command not found (try 'help')");
        }
    }
}
