#
# Copyright 2024, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Generates library libuio.a to be linked against userlevel drivers
#

ifeq ($(strip $(SDDF)),)
$(error SDDF must be specified)
endif
ifeq ($(strip $(CC_USERLEVEL)),)
$(error CC_USERLEVEL must be specified)
endif

LINUX_UIO_DIR := $(abs $(dir $(last ${MAKEFILES_LIST})))
LIBVMM ?= $(realpath ${LINUX_UIO_DIR}/../../../)

LIBUIO_IMAGES := libuio.a

CFLAGS_libuio := -I$(SDDF)/include -I$(LIBVMM)/tools/linux/include

CHECK_LIBUIO_FLAGS_MD5:=.libuio_cflags-$(shell echo -- $(CFLAGS_USERLEVEL) $(CFLAGS_libuio) | shasum | sed 's/ *-//')

$(CHECK_LIBUIO_FLAGS_MD5):
	-rm -f .libuio_cflags-*
	touch $@


libuio.a: libuio.o
	ar rv $@ $^

libuio.o: $(CHECK_LIBUIO_FLAGS_MD5)
libuio.o: $(LIBVMM)/tools/linux/uio/libuio.c
	$(CC_USERLEVEL) $(CFLAGS_USERLEVEL) $(CFLAGS_libuio) -o $@ -c $<

init.o: $(CHECK_LIBUIO_FLAGS_MD5)
init.o: $(LIBVMM)/tools/linux/uio/init.c
	$(CC_USERLEVEL) $(CFLAGS_USERLEVEL) $(CFLAGS_libuio) -o $@ -c $<

clean::
	rm -f libuio.[od] init.[od] .libuio_cflags-*

clobber::
	rm -f $(LIBUIO_IMAGES)

-include libuio.d
-include init.d
