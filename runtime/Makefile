CC := gcc
CFLAGS := -ffreestanding -nostdlib -fno-pic -fno-pie -fno-stack-protector \
          -fno-stack-check -fno-common -fno-builtin -std=gnu11 -O2 -g
TARGET := runtime

all: $(TARGET)

$(TARGET): linux_runtime_main.o linux_runtime.o linux_syscall.o bsd_syscall.o elf_loader.o
	$(CC) $(CFLAGS) linux_runtime_main.o linux_runtime.o linux_syscall.o bsd_syscall.o elf_loader.o -o $(TARGET)

linux_runtime_main.o: linux_runtime_main.c linux_runtime.h linux_syscall.h bsd_syscall.h
	$(CC) $(CFLAGS) -c linux_runtime_main.c -o linux_runtime_main.o

linux_runtime.o: linux_runtime.c linux_runtime.h linux_syscall.h
	$(CC) $(CFLAGS) -c linux_runtime.c -o linux_runtime.o

linux_syscall.o: linux_syscall.c linux_runtime.h linux_syscall.h
	$(CC) $(CFLAGS) -c linux_syscall.c -o linux_syscall.o

bsd_syscall.o: bsd_syscall.c bsd_syscall.h linux_runtime.h linux_syscall.h
	$(CC) $(CFLAGS) -c bsd_syscall.c -o bsd_syscall.o

elf_loader.o: elf_loader.c elf_loader.h
	$(CC) $(CFLAGS) -c elf_loader.c -o elf_loader.o

clean:
	rm -f *.o $(TARGET) runtime_test

test: runtime_test
	./runtime_test
	@echo "[runtime] PASS: all checks"

runtime_test: runtime_test.o linux_runtime.o linux_syscall.o bsd_syscall.o elf_loader.o
	$(CC) $(CFLAGS) runtime_test.o linux_runtime.o linux_syscall.o bsd_syscall.o elf_loader.o -o runtime_test

runtime_test.o: runtime_test.c linux_runtime.h linux_syscall.h bsd_syscall.h
	$(CC) $(CFLAGS) -c runtime_test.c -o runtime_test.o

.PHONY: all clean test