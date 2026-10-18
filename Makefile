# Helix OS — top-level build
#
#   make            build the bootable disk image (includes FreeDoom Phase 1)
#   make kernel     build only the kernel ELF
#   make deps       download the Limine bootloader binaries
#   make run        boot the image in QEMU (serial on your terminal)
#   make test       boot headlessly and verify the boot banner
#   make clean      remove build output
#
# The first default image build downloads pinned FreeDoom game data. Override
# DOOM_WAD to use a different IWAD. Every path is overridable, which is how the sandbox builds out-of-tree:
#   make BUILD_DIR=/home/user/helix-build test

BUILD_DIR ?= $(CURDIR)/build

export BUILD_DIR

LIMINE_VERSION := v12.9.3
LIMINE_URL     := https://github.com/Limine-Bootloader/Limine/releases/download/$(LIMINE_VERSION)/limine-binary.tar.gz
DEPS_DIR       := $(BUILD_DIR)/deps
LIMINE_DIR     := $(DEPS_DIR)/limine-binary
IMAGE          := $(BUILD_DIR)/helix.img
FREEDOOM_VERSION := 0.13.0
FREEDOOM_ZIP     := $(DEPS_DIR)/freedoom-$(FREEDOOM_VERSION).zip
FREEDOOM_DIR     := $(DEPS_DIR)/freedoom-$(FREEDOOM_VERSION)
FREEDOOM_WAD     := $(FREEDOOM_DIR)/freedoom1.wad

ifeq ($(origin DOOM_WAD),undefined)
ifneq ($(wildcard $(CURDIR)/freedoom1.wad),)
DOOM_WAD := $(CURDIR)/freedoom1.wad
DOOM_WAD_DEP :=
else
DOOM_WAD := $(FREEDOOM_WAD)
DOOM_WAD_DEP := freedoom
endif
DOOM_LICENSE := $(CURDIR)/third_party/freedoom/COPYING.adoc
else
DOOM_LICENSE ?=
DOOM_WAD_DEP :=
endif

.PHONY: all kernel deps freedoom image iso disks run test clean

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

freedoom: $(FREEDOOM_WAD)

$(FREEDOOM_ZIP):
	@mkdir -p "$(DEPS_DIR)"
	@echo "[deps] downloading Freedoom $(FREEDOOM_VERSION)"
	@curl -fL --retry 2 --silent --show-error -o "$@.tmp" \
	 "https://github.com/freedoom/freedoom/releases/download/v$(FREEDOOM_VERSION)/freedoom-$(FREEDOOM_VERSION).zip"
	@echo "3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59  $@.tmp" | shasum -a 256 -c -
	@mv "$@.tmp" "$@"

$(FREEDOOM_WAD): $(FREEDOOM_ZIP)
	@mkdir -p "$(FREEDOOM_DIR)"
	@unzip -p "$(FREEDOOM_ZIP)" "freedoom-$(FREEDOOM_VERSION)/freedoom1.wad" > "$@.tmp"
	@test "$$(wc -c < "$@.tmp" | tr -d ' ')" = 28817756 || { rm -f "$@.tmp"; echo "[deps] unexpected Freedoom IWAD size" >&2; exit 1; }
	@mv "$@.tmp" "$@"

image: kernel deps $(DOOM_WAD_DEP)
	@BUILD_DIR="$(BUILD_DIR)" \
	 KERNEL="$(BUILD_DIR)/kernel" \
	 LIMINE_DIR="$(LIMINE_DIR)" \
	 IMAGE="$(IMAGE)" \
	 LIMINE_CONF="$(CURDIR)/limine.conf" \
	 DOOM_WAD="$(DOOM_WAD)" \
	 DOOM_LICENSE="$(DOOM_LICENSE)" \
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

clean:
	@$(MAKE) -C kernel BUILD_DIR="$(BUILD_DIR)" clean
	@rm -rf "$(BUILD_DIR)"
