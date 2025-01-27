#
# Copyright 2024, UNSW (ABN 57 195 873 179)
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Snippet to build libvmm.a, to be included in a full-system Makefile.
#

LIBVMM_DIR :=  $(abspath $(dir $(lastword ${MAKEFILE_LIST})))

GIC_V3_BOARDS := imx8mm_evk maaxboard
ifeq ($(filter ${MICROKIT_BOARD},${GIC_V3_BOARDS}),)
	VGIC := GIC_V2
	VGIC_FILES := src/arch/aarch64/vgic/vgic_v2.c
else
	VGIC := GIC_V3
	VGIC_FILES := src/arch/aarch64/vgic/vgic_v3.c
endif

AARCH64_FILES := src/arch/aarch64/fault.c \
		 src/arch/aarch64/linux.c \
		 src/arch/aarch64/linux.c \
		 src/arch/aarch64/psci.c \
		 src/arch/aarch64/smc.c \
		 src/arch/aarch64/tcb.c \
		 src/arch/aarch64/vcpu.c \
		 src/arch/aarch64/virq.c \
		 src/arch/aarch64/vgic/vgic.c \
		 ${VGIC_FILES}

# VIRTIO MMIO depends on sddf
ifeq ($(strip $(SDDF)),)
    $(error libvmm needs the location of the SDDF to build virtIO components)
endif

# we need ${SDDF} for virtIO; we need ${LIBVMM} for all
# the libvmm api interfaces
ifeq ($(findstring ${SDDF}/include, ${CFLAGS}),)
CFLAGS += -I${SDDF}/include
endif
ifeq ($(findstring ${LIBVMM_DIR}/include,${CFLAGS}),)
CFLAGS += -I${LIBVMM_DIR}/include
endif

ARCH_INDEP_FILES := src/util/printf.c \
		    src/util/util.c \
		    src/virtio/block.c \
		    src/virtio/console.c \
		    src/virtio/mmio.c \
		    src/virtio/net.c \
		    src/virtio/sound.c \
		    src/guest.c \
			src/uio/uio.c \

CFILES := ${AARCH64_FILES} ${ARCH_INDEP_FILES}
OBJECTS := $(subst src,libvmm,${CFILES:.c=.o})


# Generate dependencies automatically
CFLAGS += -MD

# Force rebuid if CFLAGS changes.
# This will pick up (among other things} changes
# to Microkit BOARD and CONFIG.
CHECK_LIBVMM_CFLAGS:=.libvmm_cflags.$(shell echo ${CFLAGS} | shasum | sed 's/ *-$$//')
.libvmm_cflags.%:
	rm -f .libvmm_cflags.*
	echo ${CFLAGS} > $@

# This is ugly, but needed to distinguish  directories in the BUILD area
# from directories in the source area.
libvmm/arch/aarch64/vgic:
	mkdir -p libvmm/arch/aarch64/vgic/
	mkdir -p libvmm/util
	mkdir -p libvmm/virtio
	mkdir -p libvmm/uio

libvmm.a: ${OBJECTS}
	ar rv $@ $^

${OBJECTS}: ${SDDF}/include
${OBJECTS}: ${CHECK_LIBVMM_CFLAGS} |libvmm/arch/aarch64/vgic

libvmm/%.o: src/%.c
	${CC} ${CFLAGS} -c -o $@ $<

-include ${OBJECTS:.o=.d}

clean::
	rm -f ${OBJECTS} ${OBJECTS:.c=.d}

clobber:: clean
	rmdir src/arch/aarch64/vgic
	rmdir src/util
	rmdir src/virtio
	rmdir src/uio
