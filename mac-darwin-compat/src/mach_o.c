/* mach_o.c — Mach-O 64-bit binary parser and loader implementation.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include "../include/mach_o.h"
#include <string.h>

bool macho_validate(const void *buf, size_t size) {
    if (!buf || size < sizeof(mach_header_64_t)) {
        return false;
    }

    const mach_header_64_t *hdr = (const mach_header_64_t *)buf;
    if (hdr->magic != MH_MAGIC_64) {
        return false;
    }

    if (hdr->cputype != CPU_TYPE_X86_64) {
        return false;
    }

    if (sizeof(mach_header_64_t) + hdr->sizeofcmds > size) {
        return false;
    }

    return true;
}

int macho_parse(const void *buf, size_t size, macho_binary_t *out) {
    if (!macho_validate(buf, size) || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->raw_data = buf;
    out->raw_size = size;

    const mach_header_64_t *hdr = (const mach_header_64_t *)buf;
    memcpy(&out->header, hdr, sizeof(mach_header_64_t));

    const uint8_t *ptr = ((const uint8_t *)buf) + sizeof(mach_header_64_t);
    const uint8_t *end = ptr + hdr->sizeofcmds;

    for (uint32_t i = 0; i < hdr->ncmds && ptr + sizeof(load_command_t) <= end; i++) {
        const load_command_t *cmd = (const load_command_t *)ptr;
        if (cmd->cmdsize < sizeof(load_command_t) || ptr + cmd->cmdsize > end) {
            return -2; /* Malformed load command */
        }

        switch (cmd->cmd) {
            case LC_SEGMENT_64: {
                if (cmd->cmdsize < sizeof(segment_command_64_t)) return -3;
                if (out->segment_count < MAX_MACHO_SEGMENTS) {
                    macho_segment_t *seg = &out->segments[out->segment_count++];
                    memcpy(&seg->cmd, ptr, sizeof(segment_command_64_t));

                    const section_64_t *sects = (const section_64_t *)(ptr + sizeof(segment_command_64_t));
                    uint32_t num_sects = seg->cmd.nsects;
                    if (num_sects > (MAX_MACHO_SECTIONS / 2)) {
                        num_sects = (MAX_MACHO_SECTIONS / 2);
                    }
                    for (uint32_t s = 0; s < num_sects; s++) {
                        memcpy(&seg->sections[s], &sects[s], sizeof(section_64_t));
                    }
                    seg->section_count = num_sects;
                }
                break;
            }

            case LC_MAIN: {
                if (cmd->cmdsize >= sizeof(entry_point_command_t)) {
                    const entry_point_command_t *ep = (const entry_point_command_t *)ptr;
                    out->entrypoint = ep->entryoff;
                    out->stacksize = ep->stacksize;
                    out->has_entrypoint = true;
                }
                break;
            }

            case LC_UNIXTHREAD: {
                /* LC_UNIXTHREAD contains flavor, count, and registers */
                /* cmd, cmdsize, flavor (x86_THREAD_STATE64=4), count */
                if (cmd->cmdsize >= 16 + sizeof(x86_thread_state64_t)) {
                    const x86_thread_state64_t *state = (const x86_thread_state64_t *)(ptr + 16);
                    out->entrypoint = state->rip;
                    out->has_entrypoint = true;
                }
                break;
            }

            case LC_LOAD_DYLINKER: {
                if (cmd->cmdsize > 12) {
                    uint32_t str_off = *(const uint32_t *)(ptr + 8);
                    if (str_off < cmd->cmdsize) {
                        const char *s = (const char *)(ptr + str_off);
                        size_t maxlen = cmd->cmdsize - str_off;
                        size_t l = 0;
                        while (l < maxlen && s[l] && l < sizeof(out->dylinker) - 1) {
                            out->dylinker[l] = s[l];
                            l++;
                        }
                        out->dylinker[l] = '\0';
                    }
                }
                break;
            }

            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB: {
                if (cmd->cmdsize >= sizeof(dylib_command_t) && out->dylib_count < MAX_MACHO_DYLIBS) {
                    const dylib_command_t *dylib = (const dylib_command_t *)ptr;
                    if (dylib->name_offset < cmd->cmdsize) {
                        const char *s = (const char *)(ptr + dylib->name_offset);
                        size_t maxlen = cmd->cmdsize - dylib->name_offset;
                        char *dst = out->dylibs[out->dylib_count++];
                        size_t l = 0;
                        while (l < maxlen && s[l] && l < 255) {
                            dst[l] = s[l];
                            l++;
                        }
                        dst[l] = '\0';
                    }
                }
                break;
            }

            case LC_SYMTAB: {
                if (cmd->cmdsize >= sizeof(symtab_command_t)) {
                    const symtab_command_t *sym = (const symtab_command_t *)ptr;
                    out->symoff = sym->symoff;
                    out->nsyms = sym->nsyms;
                    out->stroff = sym->stroff;
                    out->strsize = sym->strsize;
                }
                break;
            }

            default:
                break;
        }

        ptr += cmd->cmdsize;
    }

    /* If entrypoint was set via LC_MAIN, adjust relative to __TEXT vmaddr */
    if (out->has_entrypoint) {
        const macho_segment_t *text = macho_find_segment(out, "__TEXT");
        if (text && out->entrypoint < text->cmd.vmaddr) {
            out->entrypoint += text->cmd.vmaddr;
        }
    }

    return 0;
}

const macho_segment_t *macho_find_segment(const macho_binary_t *bin, const char *segname) {
    if (!bin || !segname) return NULL;
    for (uint32_t i = 0; i < bin->segment_count; i++) {
        if (strncmp(bin->segments[i].cmd.segname, segname, 16) == 0) {
            return &bin->segments[i];
        }
    }
    return NULL;
}

const section_64_t *macho_find_section(const macho_binary_t *bin, const char *segname, const char *sectname) {
    const macho_segment_t *seg = macho_find_segment(bin, segname);
    if (!seg || !sectname) return NULL;

    for (uint32_t i = 0; i < seg->section_count; i++) {
        if (strncmp(seg->sections[i].sectname, sectname, 16) == 0) {
            return &seg->sections[i];
        }
    }
    return NULL;
}

bool macho_get_entrypoint(const macho_binary_t *bin, uint64_t *out_entry) {
    if (!bin || !out_entry || !bin->has_entrypoint) {
        return false;
    }
    *out_entry = bin->entrypoint;
    return true;
}

int macho_load_image(const macho_binary_t *bin, void *memory_base, size_t memory_size) {
    if (!bin || !memory_base) return -1;

    /* Base address for __PAGEZERO or first segment */
    uint64_t min_vmaddr = (uint64_t)-1;
    uint64_t max_vmaddr = 0;

    for (uint32_t i = 0; i < bin->segment_count; i++) {
        const segment_command_64_t *cmd = &bin->segments[i].cmd;
        if (cmd->vmsize == 0) continue;
        if (cmd->vmaddr < min_vmaddr) min_vmaddr = cmd->vmaddr;
        if (cmd->vmaddr + cmd->vmsize > max_vmaddr) max_vmaddr = cmd->vmaddr + cmd->vmsize;
    }

    if (min_vmaddr == (uint64_t)-1) return -2;
    size_t total_span = (size_t)(max_vmaddr - min_vmaddr);
    if (total_span > memory_size) return -3; /* Not enough room */

    uint8_t *base = (uint8_t *)memory_base;
    memset(base, 0, total_span);

    const uint8_t *raw = (const uint8_t *)bin->raw_data;

    for (uint32_t i = 0; i < bin->segment_count; i++) {
        const segment_command_64_t *cmd = &bin->segments[i].cmd;
        if (cmd->vmsize == 0) continue;
        if (strncmp(cmd->segname, "__PAGEZERO", 16) == 0) continue;

        uint64_t seg_offset = cmd->vmaddr - min_vmaddr;
        if (cmd->filesize > 0 && cmd->fileoff < bin->raw_size) {
            size_t copy_size = cmd->filesize;
            if (cmd->fileoff + copy_size > bin->raw_size) {
                copy_size = bin->raw_size - cmd->fileoff;
            }
            if (seg_offset + copy_size <= memory_size) {
                memcpy(base + seg_offset, raw + cmd->fileoff, copy_size);
            }
        }
    }

    return 0;
}

const char *macho_lookup_symbol(const macho_binary_t *bin, uint32_t index, uint64_t *out_val) {
    if (!bin || !bin->raw_data || index >= bin->nsyms) return NULL;
    if (bin->symoff + bin->nsyms * sizeof(nlist_64_t) > bin->raw_size) return NULL;
    if (bin->stroff + bin->strsize > bin->raw_size) return NULL;

    const uint8_t *raw = (const uint8_t *)bin->raw_data;
    const nlist_64_t *syms = (const nlist_64_t *)(raw + bin->symoff);
    const char *strings = (const char *)(raw + bin->stroff);

    const nlist_64_t *s = &syms[index];
    if (out_val) *out_val = s->n_value;
    if (s->n_strx < bin->strsize) {
        return strings + s->n_strx;
    }
    return NULL;
}
