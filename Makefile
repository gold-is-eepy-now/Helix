# Helix OS — top-level build
#
#   make            build the bootable disk image (default target)
#   make kernel     build only the kernel ELF
#   make deps       download the Limine bootloader binaries
#   make linux-submodule   initialize the optional Linux kernel submodule
#   make linux-drivers     build optional Linux drivers from the submodule
#   make run        boot the image in QEMU (serial on your terminal)
#   make test       boot headlessly and verify the boot banner
#   make clean      remove build output
#
# Every path is overridable, which is how the sandbox builds out-of-tree:
#   make BUILD_DIR=/home/user/helix-build test
#
# Optional Linux kernel submodule for driver sources. This is an OPTIONAL
# dependency — Helix builds and boots cleanly without it. To enable it:
#   make linux-submodule
#   make linux-drivers

BUILD_DIR ?= $(CURDIR)/build

export BUILD_DIR

LIMINE_VERSION := v12.9.3
LIMINE_URL     := https://github.com/Limine-Bootloader/Limine/releases/download/$(LIMINE_VERSION)/limine-binary.tar.gz
DEPS_DIR       := $(BUILD_DIR)/deps
LIMINE_DIR     := $(DEPS_DIR)/limine-binary
IMAGE          := $(BUILD_DIR)/helix.img

# Optional Linux kernel submodule paths
LINUX_SUBMODULE_DIR ?= third-party/linux
LINUX_SUBMODULE_INITIALIZED ?= $(shell test -d "$(LINUX_SUBMODULE_DIR)" && echo yes || echo no)

.PHONY: all kernel deps linux-submodule linux-drivers linux-drivers-ethernet linux-drivers-wifi linux-drivers-graphics linux-drivers-sound image iso disks run test clean install userspace-apps img-with-linux-drivers

all: image

img-with-linux-drivers: linux-submodule linux-drivers all
	@make image

install: kernel deps
	@BUILD_DIR="$(BUILD_DIR)" sh tools/install.sh

kernel:
	@$(MAKE) -C kernel BUILD_DIR="$(BUILD_DIR)"

deps: $(LIMINE_DIR)/BOOTX64.EFI

$(LIMINE_DIR)/BOOTX64.EFI:
	@mkdir -p "$(DEPS_DIR)"
	@echo "[deps] downloading Limine $(LIMINE_VERSION)"
	@curl -fL --silent --show-error -o "$(DEPS_DIR)/limine-binary.tar.gz" "$(LIMINE_URL)"
	@tar xzf "$(DEPS_DIR)/limine-binary.tar.gz" -C "$(DEPS_DIR)"
	@echo "[deps] Limine ready in $(LIMINE_DIR)"

# Initialize the optional Linux kernel submodule (git submodule update --init).
# This is a no-op if the submodule is already present. It is required before
# building optional drivers.
linux-submodule:
	@if [ "$(LINUX_SUBMODULE_INITIALIZED)" != "yes" ]; then \
		echo "[submodule] Initializing Linux kernel submodule at $(LINUX_SUBMODULE_DIR)..."; \
		git submodule update --init --recursive $(LINUX_SUBMODULE_DIR); \
		echo "[submodule] Linux kernel submodule ready."; \
	else \
		echo "[submodule] Linux kernel submodule already initialized."; \
	fi

# Build optional Linux drivers from the submodule. Requires `make linux-submodule`
# first. The drivers are compiled into a static library in runtime/optional_drivers/.
linux-drivers: linux-submodule
	@if [ "$(LINUX_SUBMODULE_INITIALIZED)" != "yes" ]; then \
		echo "[linux-drivers] ERROR: Linux kernel submodule not initialized."; \
		echo "[linux-drivers] Run 'make linux-submodule' first."; \
		exit 1; \
	fi
	@echo "[linux-drivers] Building optional drivers from $(LINUX_SUBMODULE_DIR)..."
	@$(MAKE) -C runtime LIB_SRCDIR="$(LINUX_SUBMODULE_DIR)" optional-drivers

# Per-driver-category targets (all require the submodule first)
linux-drivers-ethernet: linux-submodule
	@$(MAKE) -C runtime LIB_SRCDIR="$(LINUX_SUBMODULE_DIR)" optional-drivers

linux-drivers-wifi: linux-submodule
	@$(MAKE) -C runtime LIB_SRCDIR="$(LINUX_SUBMODULE_DIR)" optional-drivers

linux-drivers-graphics: linux-submodule
	@$(MAKE) -C runtime LIB_SRCDIR="$(LINUX_SUBMODULE_DIR)" optional-drivers

linux-drivers-sound: linux-submodule
	@$(MAKE) -C runtime LIB_SRCDIR="$(LINUX_SUBMODULE_DIR)" optional-drivers

# Build userspace apps (dhcp, wifi, wget, wav, volume)
userspace-apps:
	@$(MAKE) -C runtime LIB_SRCDIR="$(LINUX_SUBMODULE_DIR)" userspace-apps

image: kernel deps
	@BUILD_DIR="$(BUILD_DIR)" \
	 KERNEL="$(BUILD_DIR)/kernel" \
	 LIMINE_DIR="$(LIMINE_DIR)" \
	 IMAGE="$(IMAGE)" \
	 LIMINE_CONF="$(CURDIR)/limine.conf" \
	 DOOM_WAD="$(DOOM_WAD)" \
	 sh tools/mkimage.sh

# Bootable UEFI CD/DVD image (attach as an optical drive).
iso: image
	@BUILD_DIR="$(BUILD_DIR)" \
	 KERNEL="$(BUILD_DIR)/kernel" \
	 LIMINE_DIR="$(LIMINE_DIR)" \
	 ISO="$(BUILD_DIR)/helix.iso" \
	 LIMINE_CONF="$(CURDIR)/limine.conf" \
	 sh tools/mkiso.sh

# Hypervisor disk formats (VDI/VMDK/VHD) converted from the raw image.
disks: image
	@sh tools/mkdisk.sh "$(IMAGE)" "$(BUILD_DIR)"

run: image
	@sh tools/run-qemu.sh "$(IMAGE)"

test: image
	@BUILD_DIR="$(BUILD_DIR)" python3 tests/shell-test.py "$(IMAGE)"
	@$(MAKE) -C runtime test

clean:
	@$(MAKE) -C kernel BUILD_DIR="$(BUILD_DIR)" clean
	@$(MAKE) -C runtime clean
	@rm -rf "$(BUILD_DIR)"