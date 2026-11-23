# Makefile — i686 kernel, stage I0. Builds build/i686/kernel.elf (ELF32, Multiboot 1).
# Separate from the x86_64 kernel; it shares no code with it.
CC      ?= gcc
LD      ?= ld
BUILD   ?= ../build/i686

CFLAGS  := -m32 -march=i686 -std=gnu11 -O2 -g -ffreestanding -fno-stack-protector \
           -fno-pic -fno-pie -fno-builtin -fno-common -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
           -Wall -Wextra -Werror
ASFLAGS := -m32 -march=i686
LDFLAGS := -m elf_i386 -nostdlib -static -z noexecstack -T linker.ld

OBJS := $(BUILD)/start.o $(BUILD)/main.o

.PHONY: all clean
all: $(BUILD)/kernel.elf

$(BUILD)/start.o: start.S
	@mkdir -p $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/main.o: main.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@echo "[i686] built $@"

clean:
	rm -rf $(BUILD)
