# Helix OS — kernel build
#
# Produces a freestanding x86_64 ELF kernel that the Limine bootloader can
# load. Nothing here links against a host libc: `-ffreestanding -nostdlib`
# means the only code in the binary is ours plus whatever the compiler emits.
#
# Usage (usually driven by the top-level Makefile):
#   make            build $(BUILD_DIR)/kernel
#   make clean

BUILD_DIR ?= build
CC        ?= gcc
LD        ?= ld
.DEFAULT_GOAL := all

# GNU Make splits target names on whitespace, so a BUILD_DIR containing a
# space silently breaks every pattern rule below. The top-level Makefile
# passes a relative path (../build) for exactly this reason.
empty :=
space := $(empty) $(empty)
ifneq (,$(findstring $(space),$(BUILD_DIR)))
$(error BUILD_DIR must not contain spaces: $(BUILD_DIR))
endif

SRC_DIR := src
INC_DIR := include
KERNEL  := $(BUILD_DIR)/kernel

# Freestanding C for bare metal. The flags matter:
#   -ffreestanding        no assumptions about a hosted C library
#   -fno-stack-protector  no guard checks (there is no runtime to call)
#   -U_FORTIFY_SOURCE     no __memcpy_chk/__memmove_chk (Ubuntu gcc enables
#                         _FORTIFY_SOURCE=2 by default at -O2; those wrappers
#                         live in libc, which a freestanding kernel has none of)
#   -fno-pic/-fno-pie     kernel is linked at a fixed high address
#   -mno-red-zone         the red zone is unsafe on interrupt stacks
#   -mno-sse/-mno-mmx/-mno-80387 / -mgeneral-regs-only
#                         never touch FPU/SIMD state we have not saved
#   -mcmodel=kernel       address code/data in the top 2 GiB
CFLAGS := -std=gnu11 -O2 -g \
          -MMD -MP \
          -Wall -Wextra \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-stack-check \
          -U_FORTIFY_SOURCE -fno-common -fno-PIC -fno-PIE \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -mno-red-zone -mno-mmx -mno-sse -mno-80387 -mgeneral-regs-only \
          -m64 -march=x86-64 -mcmodel=kernel \
          -I$(INC_DIR)

# Link with the kernel's own linker script, no startup files, no libc.
LDFLAGS := -nostdlib -nostartfiles -static -no-pie \
           -Wl,-T,linker.ld \
           -Wl,-z,max-page-size=0x1000 \
           -Wl,-z,noexecstack \
           -Wl,--build-id=none

# Toolchain selection. The host gcc works on Linux. On macOS, Apple's toolchain
# cannot emit ELF (its ld is Mach-O only), so install a cross-compiler and pass
# it in:
#     make CC=x86_64-elf-gcc
# or use LLVM:
#     make TOOLCHAIN=llvm
TOOLCHAIN ?= gcc
ifeq ($(TOOLCHAIN),llvm)
    CC := clang --target=x86_64-unknown-none
    LDFLAGS += -fuse-ld=lld
    # clang does not accept -mgeneral-regs-only on every version; the explicit
    # -mno-sse / -mno-mmx / -mno-80387 already keep FPU and SIMD state out.
    CFLAGS := $(filter-out -mgeneral-regs-only,$(CFLAGS))
endif

C_SOURCES := $(wildcard $(SRC_DIR)/*.c)
S_SOURCES := $(wildcard $(SRC_DIR)/*.S)

# The userland shell: a static ring-3 ELF built from userspace/, then embedded
# in the kernel as a blob (user_shell_elf_start .. user_shell_elf_end). The
# embedding is a generated .S file so the path is always the one make used.
USER_DIR    := ../userspace
USER_ELF    := $(BUILD_DIR)/user/shell.elf
USER_BLOB_S := $(BUILD_DIR)/user/user_blob.S
USER_BLOB_O := $(BUILD_DIR)/obj/user_blob.o
USER_SRCS   := $(USER_DIR)/libhelix.c $(USER_DIR)/shell/shell.c
USER_CFLAGS := -std=gnu11 -O2 -m64 -march=x86-64 -mcmodel=small \
               -ffreestanding -fno-builtin -fno-stack-protector -U_FORTIFY_SOURCE \
               -fno-pic -fno-pie -fno-common -fno-asynchronous-unwind-tables \
               -fno-unwind-tables -mno-red-zone -mgeneral-regs-only -mno-sse -mno-mmx -mno-80387 \
               -Wall -Wextra -I$(USER_DIR)
USER_LDFLAGS := -nostdlib -static -no-pie \
                -Wl,-Ttext-segment=0x400000 -Wl,-z,max-page-size=0x1000 \
                -Wl,-z,separate-code -Wl,-z,noexecstack -Wl,--build-id=none

# The Linux compatibility layer (docs/08-linux-compat.md): glue and drivers that
# are compiled with the kernel and use the compat headers in compat/include.
COMPAT_DIR     := ../compat
COMPAT_SOURCES := $(wildcard $(COMPAT_DIR)/src/*.c)
COMPAT_OBJECTS := $(patsubst $(COMPAT_DIR)/src/%.c,$(BUILD_DIR)/obj/compat_%.o,$(COMPAT_SOURCES))

OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/obj/%.o,$(C_SOURCES)) \
           $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/obj/%.o,$(S_SOURCES)) \
           $(USER_BLOB_O) $(COMPAT_OBJECTS)
DEPFILES := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/obj/%.d,$(C_SOURCES))

-include $(DEPFILES)

# Guard against the silent failure mode where the glob finds nothing and the
# link step quietly produces (or leaves) a kernel built from no sources.
ifeq ($(strip $(OBJECTS)),)
$(error No kernel sources found in $(SRC_DIR))
endif

.PHONY: all clean
all: $(KERNEL)

$(KERNEL): $(OBJECTS) linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) -o $@
	@echo "[kernel] linked $@"

$(BUILD_DIR)/obj/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/obj/compat_%.o: $(COMPAT_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(COMPAT_DIR)/include -I$(SRC_DIR) -c $< -o $@

$(BUILD_DIR)/obj/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

$(USER_ELF): $(USER_SRCS) $(USER_DIR)/libhelix.h $(USER_DIR)/shell/shell.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(USER_SRCS)
	@echo "[user] built $@"

$(USER_BLOB_S): $(USER_ELF)
	@mkdir -p $(dir $@)
	@printf '%s\n' \
	  '.section .rodata' \
	  '.balign 4096' \
	  '.global user_shell_elf_start' \
	  'user_shell_elf_start:' \
	  '.incbin "$(abspath $(USER_ELF))"' \
	  '.global user_shell_elf_end' \
	  'user_shell_elf_end:' \
	  '.section .note.GNU-stack,"",@progbits' > $@

$(USER_BLOB_O): $(USER_BLOB_S)
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

clean:
	rm -rf "$(BUILD_DIR)/obj" "$(BUILD_DIR)/kernel" "$(BUILD_DIR)/user"
