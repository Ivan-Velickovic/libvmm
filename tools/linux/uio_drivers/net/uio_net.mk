#
# Copyright 2024, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#
# This Makefile snippet builds networking UIO driver
#

ifeq ($(strip $(LIBVMM_TOOLS)),)
$(error LIBVMM_TOOLS must be specified)
endif
ifeq ($(strip $(SDDF)),)
$(error SDDF must be specified)
endif
ifeq ($(strip $(CC_USERLEVEL)),)
$(error CC_USERLEVEL must be specified)
endif

UIO_NET_IMAGES := uio_net_driver

CFLAGS_uio_net_driver := -I$(SDDF)/include -I$(LIBVMM_TOOLS)/linux/include

CHECK_UIO_NET_DRIVER_FLAGS_MD5:=.uio_net_driver_cflags-$(shell echo -- $(CFLAGS_USERLEVEL) $(CFLAGS_uio_net_driver) | shasum | sed 's/ *-//')

$(CHECK_UIO_NET_DRIVER_FLAGS_MD5):
	-rm -f .uio_net_driver_cflags-*
	touch $@

uio_net_driver: uio_net_driver.o
	$(CC_USERLEVEL) -static $(CFLAGS_USERLEVEL) $(CFLAGS_uio_net_driver) $^ -o $@

uio_net_driver.o: $(CHECK_UIO_NET_DRIVER_FLAGS_MD5)
uio_net_driver.o: $(LIBVMM_TOOLS)/linux/uio_drivers/net/main.c
	$(CC_USERLEVEL) $(CFLAGS_USERLEVEL) $(CFLAGS_uio_net_driver) -o $@ -c $<

clean::
	rm -f uio_net_driver.[od] .uio_net_driver_cflags-*

clobber::
	rm -f $(UIO_NET_IMAGES)

-include uio_net_driver.d
