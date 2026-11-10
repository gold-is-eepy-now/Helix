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

# The ring-3 programs (userspace/): static ELFs built from libhelix plus their
# source, each embedded in the kernel as a blob between user_blob_<name>_start and
# _end (the generated user_blob.S; the table is kernel/src/userprogs.c). The
# kernel installs them under /bin at boot (kernel/src/userbin.c).
USER_DIR      := ../userspace
USER_BLOB_DIR := $(BUILD_DIR)/user
USER_SH_ELF   := $(USER_BLOB_DIR)/sh.elf
USER_INIT_ELF := $(USER_BLOB_DIR)/init.elf
# Every userspace/bin/<name>.c becomes /bin/<name>. Add the name here and the .c
# file there: the rule, the embedded blob and the program table follow from it.
USER_BIN_NAMES := args env echo true false cat ls mkdir rmdir rm touch cp find edit ping
USER_BIN_ELFS  := $(foreach n,$(USER_BIN_NAMES),$(USER_BLOB_DIR)/$(n).elf)
USER_ELFS      := $(USER_SH_ELF) $(USER_INIT_ELF) $(USER_BIN_ELFS)
USER_BLOB_S   := $(USER_BLOB_DIR)/user_blob.S
USER_BLOB_O   := $(BUILD_DIR)/obj/user_blob.o
USER_LIB      := $(USER_DIR)/libhelix.c $(USER_DIR)/libhelix.h
# The runtime every ring-3 program links: libhelix plus the C library subset in
# userspace/libc (headers in userspace/include).
USER_RT_SRCS := $(USER_DIR)/libhelix.c $(wildcard $(USER_DIR)/libc/*.c)
USER_RT_HDRS := $(USER_DIR)/libhelix.h $(wildcard $(USER_DIR)/include/*.h)
USER_CFLAGS := -std=gnu11 -O2 -m64 -march=x86-64 -mcmodel=small \
               -ffreestanding -fno-builtin -fno-stack-protector -U_FORTIFY_SOURCE \
               -fno-pic -fno-pie -fno-common -fno-asynchronous-unwind-tables \
               -fno-unwind-tables -mno-red-zone -mgeneral-regs-only -mno-sse -mno-mmx -mno-80387 \
               -Wall -Wextra -I$(USER_DIR) -I$(USER_DIR)/include
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

$(USER_SH_ELF): $(USER_RT_HDRS) $(USER_RT_SRCS) $(USER_DIR)/shell/shell.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(USER_RT_SRCS) $(USER_DIR)/shell/shell.c
	@echo "[user] built $@"

$(USER_INIT_ELF): $(USER_RT_HDRS) $(USER_RT_SRCS) $(USER_DIR)/init.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $(USER_RT_SRCS) $(USER_DIR)/init.c
	@echo "[user] built $@"

define USER_BIN_RULE
$$(USER_BLOB_DIR)/$(1).elf: $$(USER_RT_HDRS) $$(USER_RT_SRCS) $$(USER_DIR)/bin/$(1).c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(USER_CFLAGS) $$(USER_LDFLAGS) -o $$@ $$(USER_RT_SRCS) $$(USER_DIR)/bin/$(1).c
	@echo "[user] built $$@"
endef
$(foreach n,$(USER_BIN_NAMES),$(eval $(call USER_BIN_RULE,$(n))))

$(USER_BLOB_S): $(USER_ELFS)
	@mkdir -p $(dir $@)
	@printf '%s\n' \
	  '.section .rodata' \
	  '.balign 4096' \
	  '.global user_blob_sh_start' \
	  'user_blob_sh_start:' \
	  '.incbin "$(abspath $(USER_SH_ELF))"' \
	  '.global user_blob_sh_end' \
	  'user_blob_sh_end:' \
	  '.balign 4096' \
	  '.global user_blob_init_start' \
	  'user_blob_init_start:' \
	  '.incbin "$(abspath $(USER_INIT_ELF))"' \
	  '.global user_blob_init_end' \
	  'user_blob_init_end:' \
	  '.section .rodata' > $@
	@for n in $(USER_BIN_NAMES); do \
	  printf '%s\n' '.balign 4096' ".global user_blob_$${n}_start" "user_blob_$${n}_start:" ".incbin \"$(abspath $(USER_BLOB_DIR))/$${n}.elf\"" ".global user_blob_$${n}_end" "user_blob_$${n}_end:" >> $@; \
	done
	@printf '%s\n' '.section .note.GNU-stack,"",@progbits' >> $@

$(USER_BLOB_O): $(USER_BLOB_S)
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

clean:
	rm -rf "$(BUILD_DIR)/obj" "$(BUILD_DIR)/kernel" "$(BUILD_DIR)/user"
