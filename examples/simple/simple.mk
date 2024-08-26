#
# Copyright 2024, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#
QEMU := qemu-system-aarch64

MICROKIT_TOOL ?= $(MICROKIT_SDK)/bin/microkit

BOARD_DIR := $(MICROKIT_SDK)/board/$(MICROKIT_BOARD)/$(MICROKIT_CONFIG)
SYSTEM_DIR := $(EXAMPLE_DIR)/board/$(MICROKIT_BOARD)
SYSTEM_FILE := $(SYSTEM_DIR)/simple.system
IMAGE_FILE := loader.img
REPORT_FILE := report.txt

vpath %.c $(LIBVMM) $(EXAMPLE_DIR)

IMAGES := vmm.elf

ifeq ($(strip $(MICROKIT_BOARD)), qemu_virt_aarch64)
	LINUX := 85000f3f42a882e4476e57003d53f2bbec8262b0-linux
	INITRD := 6dcd1debf64e6d69b178cd0f46b8c4ae7cebe2a5-rootfs.cpio.gz
else ifeq ($(strip $(MICROKIT_BOARD)), odroidc4)
	LINUX := 98d7ef6542f59df3e614fb62122d42216c36d874-linux
	INITRD := 98d7ef6542f59df3e614fb62122d42216c36d874-linux.tar.gz
else ifeq ($(strip $(MICROKIT_BOARD)), maaxboard)
else
$(error Unsupported MICROKIT_BOARD given)
endif

CFLAGS := \
	  -mstrict-align \
	  -ffreestanding \
	  -g3 -O3 -Wall \
	  -Wno-unused-function \
	  -DMICROKIT_CONFIG_$(MICROKIT_CONFIG) \
	  -DBOARD_$(MICROKIT_BOARD) \
	  -I$(BOARD_DIR)/include \
	  -I$(LIBVMM)/include \
	  -I$(SDDF)/include \
	  -MD \
	  -MP \
	  -target $(TARGET)

LDFLAGS := -L$(BOARD_DIR)/lib
LIBS := --start-group -lmicrokit -Tmicrokit.ld libvmm.a --end-group

CHECK_FLAGS_BOARD_MD5:=.board_cflags-$(shell echo -- $(CFLAGS) $(BOARD) $(MICROKIT_CONFIG) | shasum | sed 's/ *-//')

$(CHECK_FLAGS_BOARD_MD5):
	-rm -f .board_cflags-*
	touch $@

vmm.elf: vmm.o images.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

all: loader.img

-include vmm.d

$(IMAGES): libvmm.a

$(IMAGE_FILE) $(REPORT_FILE): $(IMAGES) $(SYSTEM_FILE)
	$(MICROKIT_TOOL) $(SYSTEM_FILE) --search-path $(BUILD_DIR) --board $(MICROKIT_BOARD) --config $(MICROKIT_CONFIG) -o $(IMAGE_FILE) -r $(REPORT_FILE)

${LINUX}:
	curl -L https://trustworthy.systems/Downloads/libvmm/images/${LINUX}.tar.gz -o $@
	tar xf $@

${INITRD}:
	curl -L https://trustworthy.systems/Downloads/libvmm/images/${INITRD}.tar.gz -o $@
	tar xf $@

vm.dts: $(SYSTEM_DIR)/linux.dts $(SYSTEM_DIR)/overlay.dts
	$(LIBVMM)/tools/dtscat $^ > $@

vm.dtb: vm.dts
	$(DTC) -q -I dts -O dtb $< > $@

vmm.o: $(EXAMPLE_DIR)/vmm.c $(CHECK_FLAGS_BOARD_MD5)
	$(CC) $(CFLAGS) -c -o $@ $<

images.o: $(LIBVMM)/tools/package_guest_images.S $(LINUX) $(INITRD) vm.dtb
	$(CC) -c -g3 -x assembler-with-cpp \
					-DGUEST_KERNEL_IMAGE_PATH=\"${LINUX}/linux\" \
					-DGUEST_DTB_IMAGE_PATH=\"vm.dtb\" \
					-DGUEST_INITRD_IMAGE_PATH=\"${INITRD}/rootfs.cpio.gz\" \
					-target $(TARGET) \
					$(LIBVMM)/tools/package_guest_images.S -o $@

include $(LIBVMM)/vmm.mk

qemu: $(IMAGE_FILE)
	if ! command -v $(QEMU) > /dev/null 2>&1; then echo "Could not find dependency: qemu-system-aarch64"; exit 1; fi
	$(QEMU) -machine virt,virtualization=on,highmem=off,secure=off \
			-cpu cortex-a53 \
			-serial mon:stdio \
			-device loader,file=$(IMAGE_FILE),addr=0x70000000,cpu-num=0 \
			-m size=2G \
			-nographic

clean::
	$(RM) -f *.elf .depend* $
	find . -name \*.[do] |xargs --no-run-if-empty rm

clobber:: clean
	rm -f *.a
	rm -f $(IMAGE_FILE) $(REPORT_FILE)
