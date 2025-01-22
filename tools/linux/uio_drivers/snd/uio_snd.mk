#
# Copyright 2024, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#
# This Makefile snippet builds sound UIO driver
#

ifeq ($(strip $(SDDF)),)
$(error SDDF must be specified)
endif
ifeq ($(strip $(CC_USERLEVEL)),)
$(error CC_USERLEVEL must be specified)
endif

LINUX_BLK_DIR := $(abs $(dir $(last ${MAKEFILES_LIST})))
LIBVMM ?= $(realpath ${LINUX_BLK_DIR}/../../../../)

UIO_SND_IMAGES := uio_snd_driver

CFLAGS_uio_snd_driver := -I$(SDDF)/include -I$(LIBVMM)/tools/linux/include -I$(LIBVMM)/include -MD

LIBS_uio_snd_driver := -lasound

CHECK_UIO_SND_DRIVER_FLAGS_HASH:=.uio_snd_driver_cflags-$(shell echo -- $(CFLAGS_USERLEVEL) $(CFLAGS_uio_snd_driver) | shasum | sed 's/ *-//')

$(CHECK_UIO_SND_DRIVER_FLAGS_HASH):
	-rm -f .uio_snd_driver_cflags-*
	touch $@

UIO_SND_DRV_DIR := $(LIBVMM)/tools/linux/uio_drivers/snd

CFILES_uio_snd_driver := main.c stream.c queue.c convert.c
OBJECTS_uio_snd_driver := $(CFILES_uio_snd_driver:.c=.o)
DEPENDS_uio_snd_driver := $(CFILES_uio_snd_driver:.c=.d)

OBJECTS_uio_snd_driver := $(addprefix _uio_snd_driver/,$(OBJECTS_uio_snd_driver))
DEPENDS_uio_snd_driver := $(addprefix _uio_snd_driver/,$(DEPENDS_uio_snd_driver))

_uio_snd_driver:
	mkdir -p _uio_snd_driver

uio_snd_driver: $(OBJECTS_uio_snd_driver) libuio.a
	$(CC_USERLEVEL) $(CFLAGS_USERLEVEL) $(CFLAGS_uio_snd_driver) $(LIBS_uio_snd_driver) $^ -o $@

$(OBJECTS_uio_snd_driver): |_uio_snd_driver

_uio_snd_driver/%.o: $(UIO_SND_DRV_DIR)/%.c $(CHECK_UIO_SND_DRIVER_FLAGS_HASH)
	$(CC_USERLEVEL) $(CFLAGS_USERLEVEL) $(CFLAGS_uio_snd_driver) -o $@ -c $<

clean::
	rm -rf _uio_snd_driver .uio_snd_driver_cflags-*

clobber:: clean
	rm -f $(UIO_SND_IMAGES)

-include $(DEPENDS_uio_snd_driver)
