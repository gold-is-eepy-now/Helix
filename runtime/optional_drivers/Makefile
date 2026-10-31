# Makefile for optional Linux drivers (optional dependency on third-party/linux)

# Paths
LINUX_ROOT ?= ../third-party/linux
KERNEL_SRC_DIR ?= $(LINUX_ROOT)

# Driver categories we support
ETHERNET_DRIVERS = \
	realtek/r815x.c \
	realtek/r8120.c \
	intel/e1000.c \
	intel/igb.c

WIFI_DRIVERS = \
	intel/iwlwifi.c \
	realtek/rtlwifi.c \
	brcmfmac.c \
	ath11k.c

GRAPHICS_DRIVERS = \
	drm/mesa.c \
	nvidia/zink.c

SOUND_DRIVERS = \
	hda_intel.c \
	realtek_hda.c

# Output directories
BUILD_DIR = build
DRIVER_OBJ_DIR = $(BUILD_DIR)/drivers

# Driver compilation flags (similar to kernel's CFLAGS)
CFLAGS = -std=gnu11 -O2 -g -ffreestanding -nostdlib -fno-pic -fno-pie \
	-fno-stack-protector -fno-common -fno-builtin -I$(LINUX_ROOT)/include \
	-I$(LINUX_ROOT)/arch/x86/include \
	-I$(LINUX_ROOT)/drivers/net/ethernet \
	-I$(LINUX_ROOT)/drivers/net/wireless \
	-I$(LINUX_ROOT)/drivers/gpu/drm \
	-I$(LINUX_ROOT)/sound/pci \
	-I$(LINUX_ROOT)/sound/core

# The optional driver library that implements translation from Linux driver
# interfaces to Helix's native driver framework. It defines:
#   - fd_to_handle wrappers for driver FDs
#   - syscall translation stubs that expose driver-specific ioctls, etc.
#   - entrypoint functions that allow userspace to activate optional drivers.

.PHONY: all drivers all-drivers

# The main optional drivers library target
all: drivers

# Default: build all optional drivers
all-drivers: $(BUILD_DIR)/optional_drivers.a

# Build individual driver categories
ethernet-drivers: $(BUILD_DIR)/optional_drivers.a
wifi-drivers: $(BUILD_DIR)/optional_drivers.a
graphics-drivers: $(BUILD_DIR)/optional_drivers.a
sound-drivers: $(BUILD_DIR)/optional_drivers.a

# Driver category targets for convenience
drivers: ethernet-drivers wifi-drivers graphics-drivers sound-drivers

# The static library of all optional drivers
$(BUILD_DIR)/optional_drivers.a: $(DRIVER_OBJ_DIR) | check-linux-submodule
	@echo "[optional] Archiving optional driver library..."
	ar rcs $@ $(DRIVER_OBJ_DIR)/optional_drivers/*.o

# Check that the Linux kernel submodule is initialized
check-linux-submodule:
	@if [ ! -d "$(LINUX_ROOT)" ]; then \
		echo "[optional] ERROR: Linux kernel submodule not initialized."; \
		echo "[optional] Run 'cd .. && git submodule update --init --recursive' to clone it."; \
		exit 1; \
	fi
	@if [ "$$(ls -A $(LINUX_ROOT) 2>/dev/null)" = "" ]; then \
		echo "[optional] WARNING: Linux kernel submodule exists but is empty."; \
		echo "[optional] Initializing it with 'git submodule update --init --recursive'."; \
		git submodule update --init --recursive; \
	fi

# Compile a driver source file. This uses a simple driver stub model:
# the source is adapted to Helix's driver interfaces via a translator header
# located under optional_drivers/transport/.
$(DRIVER_OBJ_DIR)/optional_drivers/%.o: $(LINUX_ROOT)/%.c optional_drivers/transport/%.h
	@mkdir -p $(DRIVER_OBJ_DIR)/optional_drivers
	$(CC) $(CFLAGS) -c $$< -o $@

# Phony targets for convenience
.PHONY: check-linux-submodule
