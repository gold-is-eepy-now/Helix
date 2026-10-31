# Helix OS — top-level build
#
#   make            build the bootable disk image (default target)
#   make kernel     build only the kernel ELF
#   make deps       download the Limine bootloader binaries
#   make run        boot the image in QEMU (serial on your terminal)
#   make test       boot headlessly and verify the boot banner
#   make clean      remove build output
#
# Every path is overridable, which is how the sandbox builds out-of-tree:
#   make BUILD_DIR=/home/user/helix-build test

BUILD_DIR ?= $(CURDIR)/build

export BUILD_DIR

LIMINE_VERSION := v12.9.3
LIMINE_URL     := https://github.com/Limine-Bootloader/Limine/releases/download/$(LIMINE_VERSION)/limine-binary.tar.gz
DEPS_DIR       := $(BUILD_DIR)/deps
LIMINE_DIR     := $(DEPS_DIR)/limine-binary
IMAGE          := $(BUILD_DIR)/helix.img

.PHONY: all kernel deps image iso disks run test clean

all: image

kernel:
	@$(MAKE) -C kernel BUILD_DIR="$(BUILD_DIR)"

deps: $(LIMINE_DIR)/BOOTX64.EFI

$(LIMINE_DIR)/BOOTX64.EFI:
	@mkdir -p "$(DEPS_DIR)"
	@echo "[deps] downloading Limine $(LIMINE_VERSION)"
	@curl -fL --silent --show-error -o "$(DEPS_DIR)/limine-binary.tar.gz" "$(LIMINE_URL)"
	@tar xzf "$(DEPS_DIR)/limine-binary.tar.gz" -C "$(DEPS_DIR)"
	@echo "[deps] Limine ready in $(LIMINE_DIR)"

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
	@rm -rf "$(BUILD_DIR)"
