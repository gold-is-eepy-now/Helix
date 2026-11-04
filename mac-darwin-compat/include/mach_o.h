/* mach_o.h — Mach-O 64-bit binary format definitions and parser.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#ifndef MAC_DARWIN_MACH_O_H
#define MAC_DARWIN_MACH_O_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mach-O 64-bit Magic Numbers */
#define MH_MAGIC_64         0xfeedfacf  /* 64-bit Mach-O native byte order */
#define MH_CIGAM_64         0xcffaedfe  /* 64-bit Mach-O reverse byte order */

/* CPU Types and Subtypes */
#define CPU_TYPE_X86_64     0x01000007
#define CPU_SUBTYPE_X86_64_ALL 3
#define CPU_SUBTYPE_LIB64   0x80000000

/* Mach-O File Types */
#define MH_OBJECT           0x1
#define MH_EXECUTE          0x2
#define MH_FVMLIB           0x3
#define MH_CORE             0x4
#define MH_PRELOAD          0x5
#define MH_DYLIB            0x6
#define MH_DYLINKER         0x7
#define MH_BUNDLE           0x8
#define MH_DSYM             0xa

/* Mach-O Flags */
#define MH_NOUNDEFS         0x00000001
#define MH_INCRLINK         0x00000002
#define MH_DYLDLINK         0x00000004
#define MH_BINDATLOAD       0x00000008
#define MH_PREBOUND         0x00000010
#define MH_SPLIT_SEGS       0x00000020
#define MH_TWOLEVEL         0x00000080
#define MH_FORCE_FLAT       0x00000100
#define MH_NOMULTIDEFS      0x00000200
#define MH_PIE              0x00200000

/* Load Command Constants */
#define LC_REQ_DYLD         0x80000000
#define LC_SEGMENT_64       0x19
#define LC_SYMTAB           0x2
#define LC_SYMSEG           0x3
#define LC_THREAD           0x4
#define LC_UNIXTHREAD       0x5
#define LC_LOAD_DYLINKER    0xe
#define LC_ID_DYLINKER      0xf
#define LC_LOAD_DYLIB       0xc
#define LC_ID_DYLIB         0xd
#define LC_LOAD_WEAK_DYLIB  (0x18 | LC_REQ_DYLD)
#define LC_DYSYMTAB         0xb
#define LC_UUID             0x1b
#define LC_RPATH            (0x1c | LC_REQ_DYLD)
#define LC_CODE_SIGNATURE   0x1d
#define LC_SEGMENT_SPLIT_INFO 0x1e
#define LC_REEXPORT_DYLIB   (0x1f | LC_REQ_DYLD)
#define LC_SOURCE_VERSION   0x2a
#define LC_DYLIB_CODE_SIGN_DRS 0x2b
#define LC_ENCRYPTION_INFO_64 0x2c
#define LC_LINKER_OPTIMIZATION_HINT 0x2e
#define LC_VERSION_MIN_MACOSX 0x24
#define LC_FUNCTION_STARTS  0x26
#define LC_MAIN             (0x28 | LC_REQ_DYLD)
#define LC_DATA_IN_CODE     0x29
#define LC_BUILD_VERSION    0x32

/* Section Types and Attributes */
#define S_REGULAR           0x0
#define S_ZEROFILL          0x1
#define S_CSTRING_LITERALS  0x2
#define S_4BYTE_LITERALS    0x3
#define S_8BYTE_LITERALS    0x4
#define S_LITERAL_POINTERS  0x5

/* Mach-O 64-bit Header */
typedef struct {
    uint32_t magic;         /* MH_MAGIC_64 */
    uint32_t cputype;       /* CPU_TYPE_X86_64 */
    uint32_t cpusubtype;    /* Machine specifier */
    uint32_t filetype;      /* MH_EXECUTE, MH_DYLIB, etc. */
    uint32_t ncmds;         /* Number of load commands */
    uint32_t sizeofcmds;    /* Size of all load commands */
    uint32_t flags;         /* Flags */
    uint32_t reserved;      /* Reserved */
} __attribute__((packed)) mach_header_64_t;

/* Generic Load Command */
typedef struct {
    uint32_t cmd;           /* Load command type */
    uint32_t cmdsize;       /* Command size in bytes */
} __attribute__((packed)) load_command_t;

/* 64-bit Segment Load Command */
typedef struct {
    uint32_t cmd;           /* LC_SEGMENT_64 */
    uint32_t cmdsize;       /* Includes section_64 headers */
    char     segname[16];   /* Segment name (__TEXT, __DATA, etc.) */
    uint64_t vmaddr;        /* Virtual memory address */
    uint64_t vmsize;        /* Virtual memory size */
    uint64_t fileoff;       /* File offset */
    uint64_t filesize;      /* File size */
    uint32_t maxprot;       /* Maximum VM protection */
    uint32_t initprot;      /* Initial VM protection */
    uint32_t nsects;        /* Number of sections */
    uint32_t flags;         /* Flags */
} __attribute__((packed)) segment_command_64_t;

/* 64-bit Section Header */
typedef struct {
    char     sectname[16];  /* Section name (__text, __data, etc.) */
    char     segname[16];   /* Segment this section belongs to */
    uint64_t addr;          /* Memory address */
    uint64_t size;          /* Section size */
    uint32_t offset;        /* File offset */
    uint32_t align;         /* Alignment (power of 2) */
    uint32_t reloff;        /* Relocation file offset */
    uint32_t nreloc;        /* Number of relocations */
    uint32_t flags;         /* Section type and attributes */
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} __attribute__((packed)) section_64_t;

/* Symbol Table Command */
typedef struct {
    uint32_t cmd;           /* LC_SYMTAB */
    uint32_t cmdsize;
    uint32_t symoff;        /* Symbol table offset */
    uint32_t nsyms;         /* Number of symbols */
    uint32_t stroff;        /* String table offset */
    uint32_t strsize;       /* String table size */
} __attribute__((packed)) symtab_command_t;

/* 64-bit Symbol Entry */
typedef struct {
    uint32_t n_strx;        /* Index into string table */
    uint8_t  n_type;        /* Type flag */
    uint8_t  n_sect;        /* Section number or NO_SECT */
    uint16_t n_desc;        /* Description */
    uint64_t n_value;       /* Value of symbol (e.g. address) */
} __attribute__((packed)) nlist_64_t;

/* Entry Point Command (LC_MAIN) */
typedef struct {
    uint32_t cmd;           /* LC_MAIN */
    uint32_t cmdsize;
    uint64_t entryoff;      /* Offset relative to __TEXT vmaddr */
    uint64_t stacksize;     /* Initial stack size (0 = default) */
} __attribute__((packed)) entry_point_command_t;

/* Thread Command (LC_UNIXTHREAD) x86_64 State */
typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rdi, rsi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags, cs, fs, gs;
} __attribute__((packed)) x86_thread_state64_t;

/* Dynamic Library Load Command */
typedef struct {
    uint32_t cmd;           /* LC_LOAD_DYLIB */
    uint32_t cmdsize;
    uint32_t name_offset;   /* String offset in command */
    uint32_t timestamp;
    uint32_t current_version;
    uint32_t compatibility_version;
} __attribute__((packed)) dylib_command_t;

/* Parsed Mach-O representation */
#define MAX_MACHO_SEGMENTS  16
#define MAX_MACHO_SECTIONS  64
#define MAX_MACHO_DYLIBS    32

typedef struct {
    segment_command_64_t cmd;
    section_64_t         sections[MAX_MACHO_SECTIONS / 2];
    uint32_t             section_count;
} macho_segment_t;

typedef struct {
    mach_header_64_t header;
    macho_segment_t  segments[MAX_MACHO_SEGMENTS];
    uint32_t         segment_count;
    char             dylinker[256];
    char             dylibs[MAX_MACHO_DYLIBS][256];
    uint32_t         dylib_count;
    uint64_t         entrypoint;     /* Virtual address of entry point */
    uint64_t         stacksize;
    bool             has_entrypoint;
    uint32_t         symoff;
    uint32_t         nsyms;
    uint32_t         stroff;
    uint32_t         strsize;
    const void      *raw_data;
    size_t           raw_size;
} macho_binary_t;

/* Public API */
bool macho_validate(const void *buf, size_t size);
int  macho_parse(const void *buf, size_t size, macho_binary_t *out);
const macho_segment_t *macho_find_segment(const macho_binary_t *bin, const char *segname);
const section_64_t *macho_find_section(const macho_binary_t *bin, const char *segname, const char *sectname);
bool macho_get_entrypoint(const macho_binary_t *bin, uint64_t *out_entry);
int  macho_load_image(const macho_binary_t *bin, void *memory_base, size_t memory_size);
const char *macho_lookup_symbol(const macho_binary_t *bin, uint32_t index, uint64_t *out_val);

#ifdef __cplusplus
}
#endif

#endif /* MAC_DARWIN_MACH_O_H */
