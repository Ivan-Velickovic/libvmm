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

You will need to create the following directory structure in the root filesystem for the sound driver VM:
```
rootfs/
├── dev/
├── sys/
├── etc/
    ├── group
├── modules/
    ├── led-class.ko
    ├── snd-hda-codec-generic.ko
    ├── snd-hda-intel.ko
    ├── snd.ko
    ├── ledtrig-audio.ko
    ├── snd-hda-codec-realtek.ko
    ├── snd-intel-dspcfg.ko
    ├── soundcore.ko
    ├── nls_base.ko
    ├── snd-hda-codec.ko
    ├── snd-pcm.ko
    ├── usbcore.ko
    ├── snd-ctl-led.ko
    ├── snd-hda-core.ko
    ├── snd-timer.ko
    ├── virtio_snd.ko
├── alsa/
├── alsactl
```

`etc/group`: This file contains the group definitions. The group `audio` should be defined in this file.
```
audio:x:0:
```

`alsa/`, `alsactl`: These files are required for the ALSA sound driver. We will be using buildroot to build these dependencies.

### Details
* Image name: `rootfs.cpio.gz`
* Config name: `buildroot_config`
* Version: 2022.08-rc2

### Instructions for reproducing

```
wget https://buildroot.org/downloads/buildroot-2022.08-rc2.tar.xz
tar xvf buildroot-2022.08-rc2.tar.xz
cp buildroot_config buildroot-2022.08-rc2/.config
make -C buildroot-2022.08-rc2 alsa-utils
```

This will generate the `alsa/` and `alsactl` files, which can be found in `buildroot-2022.08-rc2/output/target/usr/share/alsa` and `buildroot-2022.08-rc2/output/target/usr/sbin/alsactl`.
