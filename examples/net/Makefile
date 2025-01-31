#
# Copyright 2022, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#

BUILD_DIR ?= build
export MICROKIT_CONFIG ?= debug

ifeq ($(strip $(MICROKIT_SDK)),)
$(error MICROKIT_SDK must be specified)
endif
export override MICROKIT_SDK:=$(abspath ${MICROKIT_SDK})


ifeq ($(strip $(TOOLCHAIN)),)
	TOOLCHAIN := aarch64-none-elf
	export TOOLCHAIN := aarch64-none-elf
	export LIBC := $(dir $(realpath $(shell aarch64-none-elf-gcc --print-file-name libc.a)))
endif

ifeq ($(strip $(MICROKIT_BOARD)), odroidc4)
	export DRIV_DIR := meson
	export UART_DRIV_DIR := meson
	export TIMER_DRV_DIR := meson
	export CPU := cortex-a55
else
$(error Unsupported MICROKIT_BOARD given)
endif

export BUILD_DIR:=$(abspath ${BUILD_DIR})
export MICROKIT_SDK:=$(abspath ${MICROKIT_SDK})

export CC := $(TOOLCHAIN)-gcc
export CC_USERLEVEL := zig cc
export LD := $(TOOLCHAIN)-ld
export AS := $(TOOLCHAIN)-as
export AR := $(TOOLCHAIN)-ar
export DTC := dtc
export RANLIB := $(TOOLCHAIN)-ranlib
export MICROKIT_TOOL ?= $(MICROKIT_SDK)/bin/microkit
export SDDF=$(abspath ../../dep/sddf)
export LIBVMM=$(abspath ../../)

export ECHO_INCLUDE:=$(SDDF)/examples/echo_server/include

IMAGE_FILE := $(BUILD_DIR)/loader.img
REPORT_FILE := $(BUILD_DIR)/report.txt

all: ${IMAGE_FILE}

qemu ${IMAGE_FILE} ${REPORT_FILE} clean clobber: ${BUILD_DIR}/Makefile FORCE
	${MAKE}  -C ${BUILD_DIR} MICROKIT_SDK=${MICROKIT_SDK} $(notdir $@)

${BUILD_DIR}/Makefile: echo.mk
	mkdir -p ${BUILD_DIR}
	cp echo.mk $@

FORCE:
