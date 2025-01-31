#
# Copyright 2022, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#

QEMU := qemu-system-aarch64

MICROKIT_TOOL ?= $(MICROKIT_SDK)/bin/microkit
ECHO_SERVER:=${SDDF}/examples/echo_server
ECHO_SERVER_PATCH:=${LIBVMM}/examples/net
LWIPDIR:=network/ipstacks/lwip/src
BENCHMARK:=$(SDDF)/benchmark
UTIL:=$(SDDF)/util
ETHERNET_CONFIG_INCLUDE:=${ECHO_SERVER}/include/ethernet_config
SERIAL_COMPONENTS := $(SDDF)/serial/components
UART_DRIVER := $(SDDF)/drivers/serial/$(UART_DRIV_DIR)
SERIAL_CONFIG_INCLUDE:=${ECHO_SERVER_PATCH}/include/
TIMER_DRIVER:=$(SDDF)/drivers/timer/$(TIMER_DRV_DIR)
NETWORK_COMPONENTS:=$(SDDF)/network/components

BOARD_DIR := $(MICROKIT_SDK)/board/$(MICROKIT_BOARD)/$(MICROKIT_CONFIG)
SYSTEM_FILE := ${ECHO_SERVER_PATCH}/board/$(MICROKIT_BOARD)/echo_server.system
IMAGE_FILE := loader.img
REPORT_FILE := report.txt

vpath %.c ${SDDF} ${LIBVMM} ${ECHO_SERVER}

IMAGES := vmm_ethernet.elf lwip.elf benchmark.elf idle.elf network_virt_rx.elf\
	  network_virt_tx.elf copy.elf timer_driver.elf uart_driver.elf serial_virt_tx.elf

CFLAGS := -mcpu=$(CPU) \
	  -mstrict-align \
	  -ffreestanding \
	  -g3 -O3 -Wall \
	  -Wno-unused-function \
	  -DMICROKIT_CONFIG_$(MICROKIT_CONFIG) \
	  -I$(BOARD_DIR)/include \
	  -I$(SDDF)/include \
	  -I${ECHO_INCLUDE}/lwip \
	  -I${ETHERNET_CONFIG_INCLUDE} \
	  -I$(SERIAL_CONFIG_INCLUDE) \
	  -I$(LIBVMM)/tools/linux/include \
	  -I${SDDF}/$(LWIPDIR)/include \
	  -I${SDDF}/$(LWIPDIR)/include/ipv4 \
	  -MD \
	  -MP

CFLAGS_USERLEVEL := \
	-g3 \
	-O3 \
	-Wno-unused-command-line-argument \
	-Wall -Wno-unused-function \
	-D_GNU_SOURCE \
	-target aarch64-linux-gnu \
	-I$(BOARD_DIR)/include \
	-I$(SDDF)/include \
	-I${ETHERNET_CONFIG_INCLUDE}

LDFLAGS := -L$(BOARD_DIR)/lib -L${LIBC}
LIBS := --start-group -lmicrokit -Tmicrokit.ld -lc libsddf_util_debug.a --end-group

CHECK_FLAGS_BOARD_MD5:=.board_cflags-$(shell echo -- ${CFLAGS} ${BOARD} ${MICROKIT_CONFIG} | shasum | sed 's/ *-//')

${CHECK_FLAGS_BOARD_MD5}:
	-rm -f .board_cflags-*
	touch $@

%.elf: %.o
	$(LD) $(LDFLAGS) $< $(LIBS) -o $@

include ${SDDF}/${LWIPDIR}/Filelists.mk

# NETIFFILES: Files implementing various generic network interface functions
# Override version in Filelists.mk
NETIFFILES:=$(LWIPDIR)/netif/ethernet.c

# LWIPFILES: All the above.
LWIPFILES=lwip.c $(COREFILES) $(CORE4FILES) $(NETIFFILES)
LWIP_OBJS := $(LWIPFILES:.c=.o) lwip.o utilization_socket.o \
	     udp_echo_socket.o tcp_echo_socket.o

OBJS := $(LWIP_OBJS)
DEPS := $(filter %.d,$(OBJS:.o=.d))

all: loader.img

${LWIP_OBJS}: ${CHECK_FLAGS_BOARD_MD5}
lwip.elf: $(LWIP_OBJS) libsddf_util.a
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

LWIPDIRS := $(addprefix ${LWIPDIR}/, core/ipv4 netif api)
${LWIP_OBJS}: |${BUILD_DIR}/${LWIPDIRS}
${BUILD_DIR}/${LWIPDIRS}:
	mkdir -p $@

# Need to build libsddf_util_debug.a because it's included in LIBS
# for the unimplemented libc dependencies
${IMAGES}: libsddf_util_debug.a

# Build the VMM for ethernet
ETHERNET_VMM_IMAGE_DIR := ${ECHO_SERVER_PATCH}/board/$(MICROKIT_BOARD)/
ETHERNET_DTS := $(ETHERNET_VMM_IMAGE_DIR)/linux.dts
ETHERNET_DTS_OVERLAY := $(ETHERNET_VMM_IMAGE_DIR)/overlay.dts
ETHERNET_DTS_FINAL := $(ETHERNET_VMM_IMAGE_DIR)/linux_overlayed.dts
ETHERNET_DTB := ethernet_linux.dtb
ETHERNET_KERNEL := ${ETHERNET_VMM_IMAGE_DIR}/linux
ETHERNET_INITRD := ${ETHERNET_VMM_IMAGE_DIR}/rootfs.cpio.gz
ETHERNET_INITRD_FINAL := ethernet_linux_rootfs.cpio.gz
VMM_ETHERNET_OBJS := vmm_ethernet.o package_guest_ethernet_images.o

NET_DRIVER_VM_USERLEVEL := uio_net_driver
NET_DRIVER_VM_USERLEVEL_INIT := net_driver_init

$(ETHERNET_DTB): $(ETHERNET_DTS_FINAL)
	$(DTC) -q -I dts -O dtb $< > $@

${ETHERNET_DTS_FINAL}: $(ETHERNET_DTS) $(ETHERNET_DTS_OVERLAY)
	$(LIBVMM)/tools/dtscat $(ETHERNET_DTS) $(ETHERNET_DTS_OVERLAY) > $@

${ETHERNET_INITRD_FINAL}: ${ETHERNET_INITRD} $(NET_DRIVER_VM_USERLEVEL) $(NET_DRIVER_VM_USERLEVEL_INIT)
	$(LIBVMM)/tools/packrootfs ${ETHERNET_INITRD} rootfs1 -o $@ \
		--startup $(NET_DRIVER_VM_USERLEVEL_INIT) \
		--home $(NET_DRIVER_VM_USERLEVEL)

package_guest_ethernet_images.o: $(LIBVMM)/tools/package_guest_images.S \
			$(ETHERNET_KERNEL) $(ETHERNET_INITRD_FINAL) $(ETHERNET_DTB)
	$(CC) -c -g3 -x assembler-with-cpp \
					-DGUEST_KERNEL_IMAGE_PATH=\"$(ETHERNET_KERNEL)\" \
					-DGUEST_DTB_IMAGE_PATH=\"$(ETHERNET_DTB)\" \
					-DGUEST_INITRD_IMAGE_PATH=\"$(ETHERNET_INITRD_FINAL)\" \
					$(LIBVMM_DIR)/tools/package_guest_images.S -o $@

vmm_ethernet.o: ${ETHERNET_VMM_IMAGE_DIR}/vmm_ethernet.c
	$(CC) $(CFLAGS) -c $^ -o $@

vmm_ethernet.elf: ${VMM_ETHERNET_OBJS} libvmm.a
	$(RANLIB) libvmm.a
	$(LD) -L$(BOARD_DIR)/lib $^ -lmicrokit -Tmicrokit.ld libsddf_util_debug.a -o $@
	cp $@ eth_driver.elf


${IMAGE_FILE} $(REPORT_FILE): $(IMAGES) $(SYSTEM_FILE)
	$(MICROKIT_TOOL) $(SYSTEM_FILE) --search-path $(BUILD_DIR) --board $(MICROKIT_BOARD) --config $(MICROKIT_CONFIG) -o $(IMAGE_FILE) -r $(REPORT_FILE)


include ${SDDF}/util/util.mk
include ${SDDF}/network/components/network_components.mk
include ${BENCHMARK}/benchmark.mk
include ${TIMER_DRIVER}/timer_driver.mk
include ${UART_DRIVER}/uart_driver.mk
include ${SERIAL_COMPONENTS}/serial_components.mk
include ${LIBVMM}/vmm.mk
LIBVMM_TOOLS := $(LIBVMM)/tools/
include $(LIBVMM_TOOLS)/linux/uio/uio.mk
include $(LIBVMM_TOOLS)/linux/net/net_init.mk
include $(LIBVMM_TOOLS)/linux/uio_drivers/net/uio_net.mk

qemu: $(IMAGE_FILE)
	$(QEMU) -machine virt,virtualization=on \
			-cpu cortex-a53 \
			-serial mon:stdio \
			-device loader,file=$(IMAGE_FILE),addr=0x70000000,cpu-num=0 \
			-m size=2G \
			-nographic \
			-device virtio-net-device,netdev=netdev0 \
			-netdev user,id=netdev0,hostfwd=tcp::1236-:1236,hostfwd=tcp::1237-:1237,hostfwd=udp::1235-:1235 \
			-global virtio-mmio.force-legacy=false \
			-d guest_errors

clean::
	${RM} -f *.elf .depend* $
	find . -name \*.[do] |xargs --no-run-if-empty rm

clobber:: clean
	rm -f *.a
	rm -f ${IMAGE_FILE} ${REPORT_FILE}

-include $(DEPS)
