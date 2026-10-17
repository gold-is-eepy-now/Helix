/* sysinfo.c — a Neofetch-style system information panel.
 *
 * Gathers what the kernel actually knows about the machine (CPU brand via
 * CPUID, memory totals from the bootloader map, uptime from the calibrated
 * TSC, framebuffer geometry, firmware type) and prints it beside a small
 * ASCII logo. Colours are per-backend: ANSI on serial, RGB on the framebuffer.
 *
 * There is no snprintf in a freestanding kernel, so a tiny string builder
 * lives at the top.
 */
#include "sysinfo.h"

#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "cpu.h"
#include "fb.h"
#include "memory.h"
#include "time.h"
#include "version.h"

static uint64_t g_firmware_type = 0xFF;

void sysinfo_set_firmware(uint64_t firmware_type) {
    g_firmware_type = firmware_type;
}

/* ------------------------------------------------------------------ */
/* Minimal string builder                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char buf[192];
    size_t len;
} sb_t;

static void sb_reset(sb_t *s) {
    s->len = 0;
    s->buf[0] = '\0';
}

static void sb_putc(sb_t *s, char c) {
    if (s->len + 1 < sizeof(s->buf)) {
        s->buf[s->len++] = c;
        s->buf[s->len] = '\0';
    }
}

static void sb_str(sb_t *s, const char *text) {
    for (; *text != '\0'; text++) {
        sb_putc(s, *text);
    }
}

static void sb_u64(sb_t *s, uint64_t value) {
    char tmp[21];
    size_t n = 0;
    if (value == 0) {
        sb_putc(s, '0');
        return;
    }
    while (value > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n > 0) {
        sb_putc(s, tmp[--n]);
    }
}

/* ------------------------------------------------------------------ */
/* Formatting helpers                                                  */
/* ------------------------------------------------------------------ */

static void fmt_uptime(sb_t *s) {
    if (!time_available()) {
        sb_str(s, "unavailable");
        return;
    }
    uint64_t ms = time_uptime_ms();
    uint64_t secs = ms / 1000;
    uint64_t mins = secs / 60;
    uint64_t hours = mins / 60;
    if (hours > 0) {
        sb_u64(s, hours);
        sb_str(s, "h ");
        sb_u64(s, mins % 60);
        sb_str(s, "m ");
        sb_u64(s, secs % 60);
        sb_str(s, "s");
    } else if (mins > 0) {
        sb_u64(s, mins);
        sb_str(s, "m ");
        sb_u64(s, secs % 60);
        sb_str(s, "s");
    } else {
        sb_u64(s, secs);
        sb_str(s, "s");
    }
}

static void fmt_mib(sb_t *s, uint64_t bytes) {
    sb_u64(s, bytes / (1024u * 1024u));
    sb_str(s, " MiB");
}

static const char *firmware_name(uint64_t type) {
    switch (type) {
        case 0: return "x86 BIOS";
        case 1: return "UEFI (32-bit)";
        case 2: return "UEFI (64-bit)";
        case 3: return "SBI";
        default: return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* The panel                                                           */
/* ------------------------------------------------------------------ */

#define LOGO_LINES 8

static const char *const logo[LOGO_LINES] = {
    "   ##      ##   ",
    "   ##      ##   ",
    "   ##      ##   ",
    "   ###########  ",
    "   ###########  ",
    "   ##      ##   ",
    "   ##      ##   ",
    "   ##      ##   ",
};

void sysinfo_print(void) {
    char vendor[13];
    char brand[49];
    cpu_vendor(vendor);
    cpu_brand(brand);

    sb_t os_line, kernel_line, uptime_line, shell_line;
    sb_t cpu_line, mem_line, disp_line, boot_line, user_line, sep_line;
    sb_reset(&os_line);
    sb_str(&os_line, "OS:       ");
    sb_str(&os_line, HELIX_NAME);
    sb_str(&os_line, " ");
    sb_str(&os_line, HELIX_VERSION);

    sb_reset(&kernel_line);
    sb_str(&kernel_line, "Kernel:   x86_64, long mode");

    sb_reset(&uptime_line);
    sb_str(&uptime_line, "Uptime:   ");
    fmt_uptime(&uptime_line);

    sb_reset(&shell_line);
    sb_str(&shell_line, "Shell:    ");
    sb_str(&shell_line, HELIX_SHELL);

    sb_reset(&cpu_line);
    sb_str(&cpu_line, "CPU:      ");
    sb_str(&cpu_line, brand);

    sb_reset(&mem_line);
    sb_str(&mem_line, "Memory:   ");
    if (memory_available()) {
        fmt_mib(&mem_line, memory_usable_bytes());
        sb_str(&mem_line, " usable / ");
        fmt_mib(&mem_line, memory_total_bytes());
        sb_str(&mem_line, " total");
    } else {
        sb_str(&mem_line, "unknown");
    }

    sb_reset(&disp_line);
    sb_str(&disp_line, "Display:  ");
    uint64_t w, h, bpp;
    if (fb_get_info(&w, &h, &bpp)) {
        sb_u64(&disp_line, w);
        sb_str(&disp_line, "x");
        sb_u64(&disp_line, h);
        sb_str(&disp_line, "x");
        sb_u64(&disp_line, bpp);
    } else {
        sb_str(&disp_line, "serial only");
    }

    sb_reset(&boot_line);
    sb_str(&boot_line, "Boot:     Limine (");
    sb_str(&boot_line, firmware_name(g_firmware_type));
    sb_str(&boot_line, ")");

    sb_reset(&user_line);
    sb_str(&user_line, "founder@helix");
    sb_reset(&sep_line);
    sb_str(&sep_line, "-------------");

    const char *info[10] = {
        user_line.buf, sep_line.buf, os_line.buf, kernel_line.buf,
        uptime_line.buf, shell_line.buf, cpu_line.buf, mem_line.buf,
        disp_line.buf, boot_line.buf,
    };

    console_putc('\n');
    for (size_t i = 0; i < 10; i++) {
        /* Logo column. */
        if (i < LOGO_LINES) {
            console_set_fg((i % 2 == 0) ? CONSOLE_CYAN : CONSOLE_BRIGHT_MAGENTA);
            console_write(logo[i]);
            console_reset_color();
        } else {
            console_write("                 ");
        }
        /* Info column. */
        if (i == 0) {
            console_set_fg(CONSOLE_BRIGHT_GREEN);
        } else if (i == 1) {
            console_set_fg(CONSOLE_GRAY);
        }
        console_write(info[i]);
        console_reset_color();
        console_putc('\n');
    }
    console_putc('\n');
}
