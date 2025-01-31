#
# Copyright 2024, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Generates net_driver_init scripts
#

ifeq ($(strip $(MICROKIT_BOARD)),)
$(error MICROKIT_BOARD must be specified)
endif

LINUX_NET_DIR := $(abs $(dir $(last ${MAKEFILES_LIST})))
LIBVMM ?= $(realpath ${LINUX_NET_DIR}/../../../)

net_driver_init: $(LIBVMM)/tools/linux/net/board/$(MICROKIT_BOARD)/net_driver_init
	cp $^ $@

clobber::
	rm -f net_driver_init
