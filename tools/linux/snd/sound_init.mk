#
# Copyright 2024, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Generates asound.conf, snd_driver_init
#

ifeq ($(strip $(MICROKIT_BOARD)),)
$(error MICROKIT_BOARD must be specified)
endif
ifeq ($(strip $(CC_USERLEVEL)),)
$(error CC_USERLEVEL must be specified)
endif

LINUX_BLK_DIR := $(abs $(dir $(last ${MAKEFILES_LIST})))
LIBVMM ?= $(realpath ${LINUX_BLK_DIR}/../../../)

CFLAGS_uio_snd_driver_init := -I$(LIBVMM)/tools/linux/include

CHECK_UIO_SND_DRIVER_INIT_FLAGS_MD5:=.uio_snd_driver_init_cflags-$(shell echo -- $(CFLAGS_USERLEVEL) $(CFLAGS_uio_snd_driver_init) | shasum | sed 's/ *-//')

$(CHECK_UIO_SND_DRIVER_INIT_FLAGS_MD5):
	-rm -f .uio_snd_driver_init_cflags-*
	touch $@

asound.conf: $(LIBVMM)/tools/linux/snd/board/$(MICROKIT_BOARD)/asound.conf
	cp $^ $@

snd_driver_init: snd_driver_init.o init.o
	$(CC_USERLEVEL) $(CFLAGS_USERLEVEL) $(CFLAGS_uio_snd_driver_init) $^ -o $@

snd_driver_init.o: $(CHECK_UIO_SND_DRIVER_INIT_FLAGS_MD5)
snd_driver_init.o: $(LIBVMM)/tools/linux/snd/snd_driver_init.c
	$(CC_USERLEVEL) $(CFLAGS_USERLEVEL) $(CFLAGS_uio_snd_driver_init) -o $@ -c $<

clean::
	rm -f snd_driver_init.[od] .uio_snd_driver_init_cflags-*

clobber::
	rm -f asound.conf snd_driver_init

-include snd_driver_init.d
