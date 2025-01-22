#
# Copyright 2024, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Generates blk_client_init and blk_driver_init scripts
#

ifeq ($(strip $(MICROKIT_BOARD)),)
$(error MICROKIT_BOARD must be specified)
endif
ifeq ($(strip $(CC_USERLEVEL)),)
$(error CC_USERLEVEL must be specified)
endif

LINUX_BLK_DIR := $(abs $(dir $(last ${MAKEFILES_LIST})))
LIBVMM ?= $(realpath ${LINUX_BLK_DIR}/../../../)

CFLAGS_uio_blk_driver_init := -I$(LIBVMM)/tools/linux/include -DBOARD_$(MICROKIT_BOARD)

CHECK_UIO_BLK_DRIVER_INIT_FLAGS_MD5:=.uio_blk_driver_init_cflags-$(shell echo -- $(CFLAGS_USERLEVEL) $(CFLAGS_uio_blk_driver_init) | shasum | sed 's/ *-//')

$(CHECK_UIO_BLK_DRIVER_INIT_FLAGS_MD5):
	-rm -f .uio_blk_driver_init_cflags-*
	touch $@


blk_client_init: $(LIBVMM)/tools/linux/blk/blk_client_init
	cp $^ $@

blk_driver_init: blk_driver_init.o init.o
	$(CC_USERLEVEL) $(CFLAGS_USERLEVEL) $(CFLAGS_uio_blk_driver_init) $^ -o $@

blk_driver_init.o: $(CHECK_UIO_BLK_DRIVER_INIT_FLAGS_MD5)
blk_driver_init.o: $(LIBVMM)/tools/linux/blk/blk_driver_init.c
	$(CC_USERLEVEL) $(CFLAGS_USERLEVEL) $(CFLAGS_uio_blk_driver_init) -o $@ -c $<

clean::
	rm -f blk_driver_init.[od] .uio_blk_driver_init_cflags-*

clobber::
	rm -f blk_client_init blk_driver_init

-include blk_driver_init.d
