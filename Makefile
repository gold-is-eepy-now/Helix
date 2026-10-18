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
HELIX_APP      := $(BUILD_DIR)/hello.hlx
HELIX_SHELL    := $(BUILD_DIR)/shell.hlx
HELIX_APP_OBJS := $(BUILD_DIR)/hello-app.o $(BUILD_DIR)/libhelix.o
HELIX_SHELL_OBJS := $(BUILD_DIR)/shell-app.o $(BUILD_DIR)/libhelix.o
USER_CFLAGS    := -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mno-sse -mno-mmx \
	-mno-80387 -mgeneral-regs-only -m64 -march=x86-64 -Iuser/include
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

.PHONY: all kernel deps freedoom app image iso disks run test clean

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

app: $(HELIX_APP) $(HELIX_SHELL)

$(BUILD_DIR)/hello-app.o: user/hello.c user/include/helix.h
	@mkdir -p "$(BUILD_DIR)"
	$(CC) $(USER_CFLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/shell-app.o: user/shell.c user/include/helix.h
	@mkdir -p "$(BUILD_DIR)"
	$(CC) $(USER_CFLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/libhelix.o: user/libhelix.c user/include/helix.h
	@mkdir -p "$(BUILD_DIR)"
	$(CC) $(USER_CFLAGS) -c "$<" -o "$@"

$(HELIX_APP): $(HELIX_APP_OBJS) user/hello.ld
	$(CC) -nostdlib -static -no-pie -Wl,-T,user/hello.ld \
	 -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none $(HELIX_APP_OBJS) -o "$@"

$(HELIX_SHELL): $(HELIX_SHELL_OBJS) user/hello.ld
	$(CC) -nostdlib -static -no-pie -Wl,-T,user/hello.ld \
	 -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none $(HELIX_SHELL_OBJS) -o "$@"

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

image: kernel deps $(DOOM_WAD_DEP) $(HELIX_APP) $(HELIX_SHELL)
	@BUILD_DIR="$(BUILD_DIR)" \
	 KERNEL="$(BUILD_DIR)/kernel" \
	 LIMINE_DIR="$(LIMINE_DIR)" \
	 IMAGE="$(IMAGE)" \
	 HELIX_APP="$(HELIX_APP)" \
	 HELIX_SHELL="$(HELIX_SHELL)" \
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
