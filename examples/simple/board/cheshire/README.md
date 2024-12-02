# QEMU ARM virt images

## Linux kernel

### Details
* Image name: `linux`
* Config name: `linux_config`
* Git remote: https://github.com/torvalds/linux.git
* Tag: v6.5 (commit hash: `2dde18cd1d8fac735875f2e4987f11817cc0bc2c`)
* Toolchain: `riscv64-linux-gnu-`
    * Version: (Ubuntu 11.4.0-1ubuntu1~22.04) 11.4.0

You can also get the Linux config used after booting by running the following
command in userspace: `zcat /proc/config.gz`.

### Instructions for reproducing
```
git clone --depth 1 --branch v6.5 https://github.com/torvalds/linux.git
cp linux_config linux/.config
make -C linux ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- all -j$(nproc)
```

The path to the image is: `linux/arch/riscv/boot/Image`.

## Buildroot RootFS image

### Details
* Image name: `rootfs.cpio.gz`
* Config name: `buildroot_config`
* Version: 2023.08.1

### Instructions for reproducing

```
wget https://buildroot.org/downloads/buildroot-2023.08.1.tar.xz
tar xvf buildroot-2023.08.1.tar.xz
cp buildroot_config buildroot-2022.08.1/.config
make -C buildroot-2023.08.1
```

The root filesystem will be located at: `buildroot-2023.08.1/output/images/rootfs.cpio.gz` along
with the other buildroot artefacts.
