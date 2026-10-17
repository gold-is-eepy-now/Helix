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

SRC_DIR := src
INC_DIR := include
KERNEL  := $(BUILD_DIR)/kernel

# Freestanding C for bare metal. The flags matter:
#   -ffreestanding        no assumptions about a hosted C library
#   -fno-stack-protector  no guard checks (there is no runtime to call)
#   -fno-pic/-fno-pie     kernel is linked at a fixed high address
#   -mno-red-zone         the red zone is unsafe on interrupt stacks
#   -mno-sse/-mno-mmx/-mno-80387 / -mgeneral-regs-only
#                         never touch FPU/SIMD state we have not saved
#   -mcmodel=kernel       address code/data in the top 2 GiB
CFLAGS := -std=gnu11 -O2 -g \
          -Wall -Wextra \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-stack-check \
          -fno-common -fno-PIC -fno-PIE \
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

SOURCES := $(wildcard $(SRC_DIR)/*.c)OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/obj/%.o,$(SOURCES))

.PHONY: all clean
all: $(KERNEL)

$(KERNEL): $(OBJECTS) linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) -o $@
	@echo "[kernel] linked $@"

$(BUILD_DIR)/obj/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
