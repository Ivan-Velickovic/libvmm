#
# Copyright 2024, UNSW (ABN 57 195 873 179)
#
# SPDX-License-Identifier: BSD-2-Clause
#
QEMU := qemu-system-aarch64

LIBVMM_TOOLS := $(LIBVMM)/tools
MICROKIT_TOOL ?= $(MICROKIT_SDK)/bin/microkit
SDDF_INCLUDE := $(SDDF)/include/sddf
UTIL := $(SDDF)/util

UART_DRIVER := $(SDDF)/drivers/serial/$(UART_DRIVER)
SERIAL_COMPONENTS := $(SDDF)/serial/components
SND_COMPONENTS := $(SDDF)/sound/components

# This will be changed in future PR to use same header system as serial & blk.
SND_NUM_CLIENTS := -DSND_NUM_CLIENTS=3

BOARD_DIR := $(MICROKIT_SDK)/board/$(MICROKIT_BOARD)/$(MICROKIT_CONFIG)
SYSTEM_DIR := $(VIRTIO_EXAMPLE)/board/$(MICROKIT_BOARD)
SYSTEM_FILE := $(SYSTEM_DIR)/virtio-snd.system
IMAGE_FILE := loader.img
REPORT_FILE := report.txt

SND_DRIVER_VM_USERLEVEL := uio_snd_driver
SND_DRIVER_VM_USERLEVEL_INIT := snd_driver_init
SND_DRIVER_VM_USERLEVEL_ETC := asound.conf

vpath %.c $(SDDF) $(LIBVMM) $(VIRTIO_EXAMPLE) $(NETWORK_COMPONENTS)

CFLAGS := \
	  -mstrict-align \
	  -ffreestanding \
	  -g3 -O3 -Wall \
	  -Wno-unused-function \
	  -DMICROKIT_CONFIG_$(MICROKIT_CONFIG) \
	  -DBOARD_$(MICROKIT_BOARD) \
	  -I$(BOARD_DIR)/include \
	  -I$(SDDF)/include \
	  -I$(LIBVMM)/include \
	  -I$(VIRTIO_EXAMPLE)/include \
	  -I$(LIBVMM)/tools/linux/include \
	  -I$(SDDF)/$(LWIPDIR)/include \
	  -I$(SDDF)/$(LWIPDIR)/include/ipv4 \
	  -MD \
	  -MP \
	  -target $(TARGET)

CFLAGS_USERLEVEL := \
		-g3 \
		-O3 \
		-Wno-unused-command-line-argument \
		-Wall -Wno-unused-function -Werror \
		-D_GNU_SOURCE \
		-I$(VIRTIO_EXAMPLE)/include \
		-target aarch64-linux-musl \
		$(NIX_LDFLAGS) \
		$(NIX_CFLAGS_COMPILE)
#       ^^ Required for macOS / Nix cross compilation so compiler can find alsa-lib.
#          This should be removed.

LDFLAGS := -L$(BOARD_DIR)/lib
LIBS := --start-group -lmicrokit -Tmicrokit.ld libsddf_util_debug.a libvmm.a --end-group

include $(SDDF)/util/util.mk
include $(UART_DRIVER)/uart_driver.mk
include $(SERIAL_COMPONENTS)/serial_components.mk
include $(SND_COMPONENTS)/sound_components.mk
include $(LIBVMM)/vmm.mk
include $(LIBVMM_TOOLS)/linux/uio/uio.mk
include $(LIBVMM_TOOLS)/linux/snd/sound_init.mk
include $(LIBVMM_TOOLS)/linux/uio_drivers/snd/uio_snd.mk

IMAGES := client_vmm.elf snd_driver_vmm.elf \
	$(SERIAL_IMAGES) $(SND_IMAGES) uart_driver.elf

CHECK_FLAGS_BOARD_MD5:=.board_cflags-$(shell echo -- $(CFLAGS) $(BOARD) $(MICROKIT_CONFIG) | shasum | sed 's/ *-//')

$(CHECK_FLAGS_BOARD_MD5):
	-rm -f .board_cflags-*
	touch $@

%_vmm.elf: %_vm/vmm.o %_vm/images.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

all: loader.img

-include vmm.d

$(IMAGES): libsddf_util_debug.a libvmm.a

$(IMAGE_FILE) $(REPORT_FILE): $(IMAGES) $(SYSTEM_FILE)
	$(MICROKIT_TOOL) $(SYSTEM_FILE) --search-path $(BUILD_DIR) --board $(MICROKIT_BOARD) \
		--config $(MICROKIT_CONFIG) -o $(IMAGE_FILE) -r $(REPORT_FILE)

%_vm:
	mkdir -p $@

client_vm/rootfs.cpio.gz: $(SYSTEM_DIR)/client_vm/rootfs.cpio.gz |client_vm
	cp $< $@

snd_driver_vm/rootfs.cpio.gz: $(SYSTEM_DIR)/snd_driver_vm/rootfs.cpio.gz \
	$(SND_DRIVER_VM_USERLEVEL) $(SND_DRIVER_VM_USERLEVEL_INIT) |snd_driver_vm
	$(LIBVMM)/tools/packrootfs $(SYSTEM_DIR)/snd_driver_vm/rootfs.cpio.gz \
		snd_driver_vm/rootfs -o $@ \
		--init $(SND_DRIVER_VM_USERLEVEL_INIT) \
		--home $(SND_DRIVER_VM_USERLEVEL)

%_vm/vm.dts: $(SYSTEM_DIR)/%_vm/dts/linux.dts \
	$(SYSTEM_DIR)/%_vm/dts/overlays/*.dts $(CHECK_FLAGS_BOARD_MD5) |%_vm
	$(LIBVMM)/tools/dtscat $^ > $@

%_vm/vm.dtb: %_vm/vm.dts |%_vm
	$(DTC) -q -I dts -O dtb $< > $@

%_vm/vmm.o: $(VIRTIO_EXAMPLE)/%_vmm.c $(CHECK_FLAGS_BOARD_MD5) |%_vm
	$(CC) $(CFLAGS) -c -o $@ $<

%_vm/images.o: $(LIBVMM)/tools/package_guest_images.S $(CHECK_FLAGS_BOARD_MD5) \
	$(SYSTEM_DIR)/%_vm/linux %_vm/vm.dtb %_vm/rootfs.cpio.gz
	$(CC) -c -g3 -x assembler-with-cpp \
					-DGUEST_KERNEL_IMAGE_PATH=\"$(SYSTEM_DIR)/$(@D)/linux\" \
					-DGUEST_DTB_IMAGE_PATH=\"$(@D)/vm.dtb\" \
					-DGUEST_INITRD_IMAGE_PATH=\"$(@D)/rootfs.cpio.gz\" \
					-target $(TARGET) \
					$(LIBVMM)/tools/package_guest_images.S -o $@

qemu: $(IMAGE_FILE)
	[ ${MICROKIT_BOARD} = qemu_virt_aarch64 ]
	$(QEMU) -machine virt,virtualization=on,secure=off \
			-cpu cortex-a53 \
			-serial mon:stdio \
			-device loader,file=$(IMAGE_FILE),addr=0x70000000,cpu-num=0 \
			-audio driver=$(QEMU_SND_BACKEND),model=$(QEMU_SND_FRONTEND),id=$(QEMU_SND_BACKEND) \
			-m size=2G \
			-nographic

clean::
	$(RM) -f *.elf .depend* $
	find . -name \*.[do] -type f |xargs --no-run-if-empty rm

clobber:: clean
	rm -f *.a
	rm -f $(IMAGE_FILE) $(REPORT_FILE)

# These intermediate files are inferred, so will
# be deleted after a build without being marked PRECIOUS
.PRECIOUS: snd_driver_vm/vmm.o snd_driver_vm/images.o
.PRECIOUS: client_vm/vmm.o client_vm/images.o
