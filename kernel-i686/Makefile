# Makefile — i686 kernel, stage I0. Builds build/i686/kernel.elf (ELF32, Multiboot 1).
# Separate from the x86_64 kernel; it shares no code with it.
CC      ?= gcc
LD      ?= ld
BUILD   ?= ../build/i686

# Shared with the x86_64 kernel: the frame allocator and the string helpers it uses.
SHARED_SRC := ../kernel/src
SHARED_OBJS := $(BUILD)/pmm.o $(BUILD)/kstring.o $(BUILD)/heap.o

CFLAGS  := -m32 -march=i686 -std=gnu11 -O2 -g -ffreestanding -fno-stack-protector \
           -fno-pic -fno-pie -fno-builtin -fno-common -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
           -Wall -Wextra -Werror
ASFLAGS := -m32 -march=i686
LDFLAGS := -m elf_i386 -nostdlib -static -z noexecstack -T linker.ld

OBJS := $(BUILD)/start.o $(BUILD)/main.o $(BUILD)/mm_test.o $(BUILD)/paging.o $(BUILD)/vm_test.o $(SHARED_OBJS)

.PHONY: all clean
all: $(BUILD)/kernel.elf

$(BUILD)/start.o: start.S
	@mkdir -p $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/main.o: main.c ../kernel/src/pmm.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(SHARED_SRC) -c $< -o $@

$(BUILD)/mm_test.o: mm_test.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(SHARED_SRC) -c $< -o $@

$(BUILD)/pmm.o: $(SHARED_SRC)/pmm.c $(SHARED_SRC)/pmm.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(SHARED_SRC) -c $< -o $@

$(BUILD)/heap.o: $(SHARED_SRC)/heap.c $(SHARED_SRC)/heap.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(SHARED_SRC) -c $< -o $@

$(BUILD)/paging.o: paging.c paging.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vm_test.o: vm_test.c paging.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(SHARED_SRC) -c $< -o $@

$(BUILD)/kstring.o: $(SHARED_SRC)/kstring.c $(SHARED_SRC)/kstring.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(SHARED_SRC) -c $< -o $@

$(BUILD)/kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@echo "[i686] built $@"

clean:
	rm -rf $(BUILD)
