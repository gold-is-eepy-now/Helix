CC := gcc
CFLAGS := -ffreestanding -nostdlib -fno-pic -fno-pie -fno-stack-protector \
          -fno-stack-check -fno-common -fno-builtin -std=gnu11 -O2 -g
TARGET := runtime

# Optional drivers library (requires third-party/linux submodule)
OPTIONAL_DRIVERS_DIR := optional_drivers
OPTIONAL_DRIVERS_LIB := $(OPTIONAL_DRIVERS_DIR)/build/optional_drivers.a
OPTIONAL_DRIVERS_SRCS := $(OPTIONAL_DRIVERS_DIR)/ethernet.c \
                         $(OPTIONAL_DRIVERS_DIR)/wifi.c \
                         $(OPTIONAL_DRIVERS_DIR)/graphics.c \
                         $(OPTIONAL_DRIVERS_DIR)/sound.c
OPTIONAL_DRIVERS_OBJS := $(OPTIONAL_DRIVERS_SRCS:.c=.o)

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

# Optional drivers build targets
optional-drivers: $(OPTIONAL_DRIVERS_LIB)

$(OPTIONAL_DRIVERS_LIB): $(OPTIONAL_DRIVERS_OBJS)
	@mkdir -p $(OPTIONAL_DRIVERS_DIR)/build
	ar rcs $@ $(OPTIONAL_DRIVERS_OBJS)
	@echo "[runtime] Optional drivers library built: $@"

# Build individual driver objects
$(OPTIONAL_DRIVERS_DIR)/%.o: $(OPTIONAL_DRIVERS_DIR)/%.c $(OPTIONAL_DRIVERS_DIR)/transport/*.h
	@mkdir -p $(OPTIONAL_DRIVERS_DIR)/build
	$(CC) $(CFLAGS) -I$(OPTIONAL_DRIVERS_DIR)/transport -c $< -o $@

# Userspace apps
USERSpace_APPS_DIR := ../userspace/apps
USERSpace_APPS := dhcp wifi wget wav volume
USERSpace_APP_BINS := $(addprefix $(USERSpace_APPS_DIR)/,$(USERSpace_APPS))

userspace-apps: $(USERSpace_APP_BINS)

$(USERSpace_APPS_DIR)/%: $(USERSpace_APPS_DIR)/%.c
	$(CC) $(CFLAGS) -I.. -I../../runtime/optional_drivers/transport -I. $< -o $@

clean:
	rm -f *.o $(TARGET) runtime_test
	rm -rf $(OPTIONAL_DRIVERS_DIR)/build
	rm -f $(OPTIONAL_DRIVERS_OBJS)
	rm -f $(USERSpace_APP_BINS)

test: runtime_test
	./runtime_test
	@echo "[runtime] PASS: all checks"

runtime_test: runtime_test.o linux_runtime.o linux_syscall.o bsd_syscall.o elf_loader.o
	$(CC) $(CFLAGS) runtime_test.o linux_runtime.o linux_syscall.o bsd_syscall.o elf_loader.o -o runtime_test

runtime_test.o: runtime_test.c linux_runtime.h linux_syscall.h bsd_syscall.h
	$(CC) $(CFLAGS) -c runtime_test.c -o runtime_test.o

.PHONY: all clean test optional-drivers userspace-apps