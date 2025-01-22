<!--
     Copyright 2024, UNSW
     SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Odroid-C4 images

## Linux kernel

### Details
* Image name: `linux`
* Config name: `linux_config`
* Git remote: https://github.com/torvalds/linux.git
* Tag: v5.18 (commit hash: `4b0986a3613c92f4ec1bdc7f60ec66fea135991f`)
* Toolchain: `aarch64-none-elf`
    * Version: GNU Toolchain for the A-profile Architecture 10.2-2020.11 (arm-10.16)) 10.2.1 20201103

You can also get the Linux config used after booting by running the following
command in userspace: `zcat /proc/config.gz`.

### Instructions for reproducing
```
git clone --depth 1 --branch v5.18 https://github.com/torvalds/linux.git
cp linux_config linux/.config
make -C linux ARCH=arm64 CROSS_COMPILE=aarch64-none-elf- all -j$(nproc)
```

The path to the image is: `linux/arch/arm64/boot/Image`.

## RootFS image

### Details
* Image name: `rootfs.cpio.gz`

### Instructions for reproducing

You can find instructions for building a minimal root filesystem here [here](/docs/VIRTIO.md#minimal-userspace).

You will need to create the following directory structure in the root filesystem for the block driver VM:
```
rootfs/
├── dev/
├── sys/
├── modules/
    ├── virtio_blk.ko
```
