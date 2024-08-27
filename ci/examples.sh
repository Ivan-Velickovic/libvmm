#!/bin/bash

# Copyright 2024, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause

set -e

SDK_PATH=$1

[[ -z $SDK_PATH ]] && echo "usage: examples.sh [PATH TO SDK]" && exit 1
[[ ! -d $SDK_PATH ]] && echo "The path to the SDK provided does not exist: '$SDK_PATH'" && exit 1

build_simple_make() {
    BOARD=$1
    CONFIG=$2
    echo "CI|INFO: building simple example via Make with board: $BOARD and config: $CONFIG"
    BUILD_DIR="${PWD}/build/examples/simple/make/${BOARD}/${CONFIG}"
    mkdir -p ${BUILD_DIR}
    make -C examples/simple -B \
        BUILD_DIR=${BUILD_DIR} \
        MICROKIT_CONFIG=${CONFIG} \
        MICROKIT_BOARD=${BOARD} \
        MICROKIT_SDK=${SDK_PATH}
}

build_simple_zig() {
    BOARD=$1
    CONFIG=$2
    echo "CI|INFO: building simple example via Zig with board: $BOARD and config: $CONFIG"
    BUILD_DIR="${PWD}/build/examples/simple/zig/${BOARD}/${CONFIG}"
    EXAMPLE_DIR="${PWD}/examples/simple"
    pushd ${EXAMPLE_DIR}
    zig build \
        -Dsdk=${SDK_PATH} \
        -Dboard=${BOARD} \
        -Dconfig=${CONFIG} \
        -p ${BUILD_DIR}
    popd
}

build_rust() {
    echo "CI|INFO: building Rust example with config: $1"
    CONFIG=$1
    BUILD_DIR="${PWD}/build/examples/rust/qemu_virt_aarch64/${CONFIG}"
    mkdir -p ${BUILD_DIR}
    make -C examples/rust -B \
        BUILD_DIR=${BUILD_DIR} \
        CONFIG=${CONFIG} \
        MICROKIT_SDK=${SDK_PATH}
}

build_zig() {
    echo "CI|INFO: building Zig example with config: $1, Zig optimize is: $2"
    CONFIG=$1
    ZIG_OPTIMIZE=$2
    BUILD_DIR="${PWD}/build/examples/zig/qemu_virt_aarch64/${CONFIG}/${ZIG_OPTIMIZE}"
    EXAMPLE_DIR="${PWD}/examples/zig"
    mkdir -p ${BUILD_DIR}
    pushd ${EXAMPLE_DIR}
    zig build \
        -Dsdk=${SDK_PATH} \
        -Dconfig=${CONFIG} \
        -Doptimize=${ZIG_OPTIMIZE} \
        -p ${BUILD_DIR}
    popd
}

simulate_rust() {
    echo "CI|INFO: simulating Rust example with config: $1"
    BUILD_DIR="${PWD}/build/examples/rust/qemu_virt_aarch64/${CONFIG}"
    ./ci/buildroot_login.exp ${BUILD_DIR}/loader.img
}

simulate_simple_zig() {
    BOARD=$1
    CONFIG=$2
    echo "CI|INFO: simulating simple example via Zig with board: $BOARD and config: $CONFIG"
    BUILD_DIR="${PWD}/build/examples/simple/zig/${BOARD}/${CONFIG}"
    ./ci/buildroot_login.exp ${BUILD_DIR}/bin/loader.img
}

simulate_simple_make() {
    BOARD=$1
    CONFIG=$2
    echo "CI|INFO: simulating simple example via Make with board: $BOARD and config: $CONFIG"
    BUILD_DIR="${PWD}/build/examples/simple/make/${BOARD}/${CONFIG}"
    ./ci/buildroot_login.exp ${BUILD_DIR}/loader.img
}

build_virtio() {
    BOARD=$1
    CONFIG=$2
    echo "CI|INFO: building virtio example via Make with board: $BOARD and config: $CONFIG"
    BUILD_DIR="${PWD}/build/examples/virtio/make/${BOARD}/${CONFIG}"
    mkdir -p ${BUILD_DIR}
    make -C examples/virtio -B \
        BUILD_DIR=${BUILD_DIR} \
        MICROKIT_CONFIG=${CONFIG} \
        MICROKIT_BOARD=${BOARD} \
        MICROKIT_SDK=${SDK_PATH}
}

simulate_zig() {
    echo "CI|INFO: simulating Zig example with config: $1"
    BUILD_DIR="${PWD}/build/examples/zig/qemu_virt_aarch64/${CONFIG}/${ZIG_OPTIMIZE}"
    ./ci/buildroot_login.exp ${BUILD_DIR}/bin/loader.img
}

build_simple_make "qemu_virt_aarch64" "debug"
simulate_simple_make "qemu_virt_aarch64" "debug"
build_simple_make "qemu_virt_aarch64" "release"
simulate_simple_make "qemu_virt_aarch64" "release"

# @ivanv: we should incorporate the zig optimisation levels as well
build_simple_zig "qemu_virt_aarch64" "debug"
simulate_simple_zig "qemu_virt_aarch64" "debug"
build_simple_zig "qemu_virt_aarch64" "release"
simulate_simple_zig "qemu_virt_aarch64" "release"

build_simple_make "odroidc4" "debug"
build_simple_make "odroidc4" "release"

build_simple_zig "odroidc4" "debug"
build_simple_zig "odroidc4" "release"

build_simple_make "maaxboard" "debug"
build_simple_make "maaxboard" "release"

build_simple_zig "maaxboard" "debug"
build_simple_zig "maaxboard" "release"

build_rust "debug"
simulate_rust "debug"
build_rust "release"
simulate_rust "release"

# Here there are two kinds of configuration that we need to test. There is the
# configuration of Microkit itself for which we test debug and release. This
# also dictates the configuration of seL4 that is used.
# There is also the optimisations that Zig applies to the VMM code, Zig has
# three optimisation levels (other than debug), that realistically would be
# used with the "release" configuration of Microkit. For details on each
# optimisation level, see the Zig documentation.
build_zig "debug" "Debug"
simulate_zig "debug" "Debug"
build_zig "release" "ReleaseFast"
simulate_zig "release" "ReleaseFast"
build_zig "release" "ReleaseSafe"
simulate_zig "release" "ReleaseSafe"
build_zig "release" "ReleaseSmall"
simulate_zig "release" "ReleaseSmall"

build_virtio "qemu_virt_aarch64" "debug"
build_virtio "qemu_virt_aarch64" "release"
build_virtio "odroidc4" "debug"
build_virtio "odroidc4" "release"

echo ""
echo "CI|INFO: Passed all VMM tests"
