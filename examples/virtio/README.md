<!--
     Copyright 2024, UNSW
     SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Using virtIO with multiple Linux guests

This example shows off the virtIO support that libvmm provides using the
[seL4 Device Driver Framework](https://github.com/au-ts/sddf) to talk to the
actual hardware.

This example makes use of the following virtIO devices emulated by libvmm:
* console
* block

The console device is emulated by using a native driver for the hardware's UART
device from sDDF.

The block device is emulated by a virtualised driver in a separate Linux
virtual machine.

In order to show device sharing, the system has two Linux VMs that act as clients.
The two client VMs have the same resources and are identical.

The example currently works on the following platforms:
* QEMU virt AArch64
* HardKernel Odroid-C4

## Dependencies

In addition to the dependencies outlined in the top-level README, the following dependencies
are needed:
* Zig (for cross-compiling for Linux user-space)
* mkfs.fat

### Linux

Zig can be installed manually or via a package manager, see
[here](https://github.com/ziglang/zig/wiki/Install-Zig-from-a-Package-Manager).

You may already have `mkfs.fat`, if not and you use `apt` try the following command:
```sh
sudo apt-get install dosfstools
```

### macOS

Zig can be installed manually or via a package manager, see
[here](https://github.com/ziglang/zig/wiki/Install-Zig-from-a-Package-Manager).

To install `mkfs.fat` run:
```sh
brew install dosfstools
```

### Nix

The top-level `shell.nix` has everything necessary:
```sh
nix-shell ../../shell.nix
```

## Building

```sh
make MICROKIT_BOARD=<BOARD> MICROKIT_SDK=/path/to/sdk
```

Where `<BOARD>` is one of:
* `qemu_virt_aarch64`
* `odroidc4`

Other configuration options can be passed to the Makefile such as `CONFIG`
and `BUILD_DIR`, see the Makefile for details.

If you would like to simulate the QEMU board you can run the following command:
```sh
make MICROKIT_BOARD=qemu_virt_aarch64 MICROKIT_SDK=/path/to/sdk qemu
```

This will build the example code as well as run the QEMU command to simulate a
system running the whole system.

## Running

### virtIO console

This example makes use of the virtIO console device so that neither guest has access
to any serial device on the platform. The virtIO console support in libvmm talks to
a serial multiplexor which then talks to a driver for input/output to the physical
serial device.

When you boot the example, you will see different coloured output for each guest.
The Linux logs will be interleaving like so:
```
Starting klogd: OKStarting klogd:
OK
Running sysctl: Running sysctl: OK
OKSaving random seed:
Saving random seed: [    4.070358] random: crng init done
[    4.103992] random: crng init done
OK
Starting network: OK
Starting network: OK
OK

Welcome to Buildroot
buildroot login:
Welcome to Buildroot
buildroot login:
```

Initially all input is defaulted to guest 0 in red. To switch input into
the other guest 1 (green), type in `ctrl + \` followed by `1` and then `enter`.
To switch input to the block driver VM, type in `ctrl + \`, followed by `2` and `enter`.

### virtIO block

Guest 1 and guest 2 also doubles as a client in the block system that talks
virtIO to guest 3 that acts as driver with passthrough access to the block device.
The requests from both clients are multiplexed through the additional block virtualiser
component.

When you boot the example, the block driver VM will boot first. When it is ready, the
client VMs will boot together. After the client VMs boot, they will attempt to mount the
virtIO block device `/dev/vda` into `/mnt`. The kernel logs from linux will show the
virtIO drive initialising for both clients.
```
[    5.381885] virtio_blk virtio1: [vda] 2040 512-byte logical blocks (1.04 MB/1020 KiB)
[    5.325953] virtio_blk virtio1: [vda] 2040 512-byte logical blocks (1.04 MB/1020 KiB)
```

The system expects the storage device to contain an MBR partition table that contains
two partitions. Each partition is allocated to a single client. Partitions must have a
starting block number that is a multiple of sDDF block's transfer size of 4096 bytes
divided by the disk's logical size. Partitions that do not follow this restriction
are unsupported.

### QEMU set up

When running on QEMU, read and writes go to an emulated ramdisk instead of to your
local storage device. The ramdisk file supplied to QEMU is formatted during build
time to contain a FAT filesystem for both partitions.

### Hardware set up

When running on one of the supported hardware platforms, the system expects to read
and write from the SD card. You will need to format the SD card prior to booting.

