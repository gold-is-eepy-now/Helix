CC := gcc
CFLAGS := -ffreestanding -nostdlib -fno-pic -fno-pie -fno-stack-protector \
          -fno-stack-check -fno-common -fno-builtin -std=gnu11 -O2 -g
TARGET := linux_runtime

all: $(TARGET)

$(TARGET): linux_runtime_main.o linux_runtime.o linux_syscall.o elf_loader.o
	$(CC) $(CFLAGS) linux_runtime_main.o linux_runtime.o linux_syscall.o elf_loader.o -o $(TARGET)

linux_runtime_main.o: linux_runtime_main.c linux_runtime.h linux_syscall.h
	$(CC) $(CFLAGS) -c linux_runtime_main.c -o linux_runtime_main.o

linux_runtime.o: linux_runtime.c linux_runtime.h linux_syscall.h
	$(CC) $(CFLAGS) -c linux_runtime.c -o linux_runtime.o

linux_syscall.o: linux_syscall.c linux_runtime.h linux_syscall.h
	$(CC) $(CFLAGS) -c linux_syscall.c -o linux_syscall.o

elf_loader.o: elf_loader.c elf_loader.h
	$(CC) $(CFLAGS) -c elf_loader.c -o elf_loader.o

clean:
	rm -f *.o $(TARGET)

.PHONY: all clean