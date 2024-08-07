// Copyright 2024, UNSW
// SPDX-License-Identifier: BSD-2-Clause

const std = @import("std");
const libvmm_mod = @import("libvmm");
const Build = std.Build;
const LazyPath = Build.LazyPath;
const Tuple = std.meta.Tuple;

const MicrokitBoard = enum {
    qemu_virt_aarch64,
    odroidc4
};

const ConfigOptions = enum {
    debug,
    release,
    benchmark
};

const Target = struct {
    board: MicrokitBoard,
    zig_target: std.Target.Query,
};

const targets = [_]Target {
    .{
        .board = MicrokitBoard.qemu_virt_aarch64,
        .zig_target = std.Target.Query{
            .cpu_arch = .aarch64,
            .cpu_model = .{ .explicit = &std.Target.arm.cpu.cortex_a53 },
            .os_tag = .freestanding,
            .abi = .none,
            .ofmt = .elf,
        },
    },
    .{
        .board = MicrokitBoard.odroidc4,
        .zig_target = std.Target.Query{
            .cpu_arch = .aarch64,
            .cpu_model = .{ .explicit = &std.Target.arm.cpu.cortex_a55 },
            .os_tag = .freestanding,
            .abi = .none,
            .ofmt = .elf,
        },
    }
};

var libmicrokit: LazyPath = undefined;
var libmicrokit_linker_script: LazyPath = undefined;
var libmicrokit_include: LazyPath = undefined;
var microkit_board: MicrokitBoard = undefined;
var libvmm_dep: *Build.Dependency = undefined;
var sddf_dep: *Build.Dependency = undefined;

fn fileExists(path: []const u8) bool {
    std.fs.cwd().access(path, .{}) catch return false;
    return true;
}

fn findTarget(board: MicrokitBoard) std.Target.Query {
    for (targets) |target| {
        if (board == target.board) {
            return target.zig_target;
        }
    }
    std.log.err("Board '{}' is not supported\n", .{ board });
    std.posix.exit(1);
}


fn addPd(b: *Build, options: Build.ExecutableOptions) *Build.Step.Compile {
    const pd = b.addExecutable(options);
    pd.addObjectFile(libmicrokit);
    pd.setLinkerScriptPath(libmicrokit_linker_script);
    pd.addIncludePath(libmicrokit_include);

    return pd;
}

fn addVmm(
    b: *Build,
    vm_name: []const u8,
    prog_img: []const u8,
    vmm_src_file: LazyPath,
    rootfs_base: LazyPath,
    rootfs_home: []const LazyPath,
    rootfs_startup: []const LazyPath,
    dts_base: LazyPath,
    dts_overlays: []const LazyPath,
    target: Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) *Build.Step.Compile {
    const libvmm = libvmm_dep.artifact("vmm");

    const vmm = addPd(b, .{
        .name = prog_img,
        .target = target,
        .optimize = optimize,
        .strip = false,
    });

    vmm.linkLibrary(libvmm);
    vmm.addIncludePath(sddf_dep.path("include"));
    vmm.addIncludePath(b.path("include"));

    vmm.addCSourceFile(.{
        .file = vmm_src_file,
        .flags = &.{
            "-Wall",
            "-Werror",
            "-Wno-unused-function",
            "-mstrict-align",
            b.fmt("-DBOARD_{s}", .{ @tagName(microkit_board) })
        }
    });
    // Generate DTB
    const dts_name = b.fmt("{s}_{s}.dts", .{@tagName(microkit_board), vm_name});
    const dtb_name = b.fmt("{s}_{s}.dtb", .{@tagName(microkit_board), vm_name});

    _, const dts = libvmm_mod.dtscat(b, dts_base, dts_overlays);
    const dts_install_step = &b.addInstallFileWithDir(dts, .prefix, dts_name).step;

    const dtc_cmd, const dtb = libvmm_mod.compileDts(b, dts);
    dtc_cmd.step.dependOn(dts_install_step);
    const dtb_install_step = &b.addInstallFileWithDir(dtb, .prefix, dtb_name).step;

    // Pack rootfs
    const rootfs_name = b.fmt("{s}_{s}_rootfs.cpio.gz", .{@tagName(microkit_board), vm_name});
    // TODO: Investigate another way to do this, don't use /tmp?
    const rootfs_tmpdir_path = b.fmt("/tmp/{s}_{s}_rootfs", .{ @tagName(microkit_board), vm_name});
    const rootfs_output_path = b.fmt("/tmp/{s}", .{ rootfs_name });

    _, const rootfs = libvmm_mod.packrootfs(
        b,
        rootfs_base,
        rootfs_tmpdir_path,
        rootfs_output_path,
        rootfs_home,
        rootfs_startup
    );
    const rootfs_install_step = &b.addInstallFileWithDir(rootfs, .prefix, rootfs_name).step;

    const vm_images = b.addObject(.{
        .name = b.fmt("{s}_images.o", .{ vm_name }),
        .target = target,
        .optimize = optimize,
    });
    // We need to produce the rootfs before doing anything to produce guest_images
    vm_images.step.dependOn(rootfs_install_step);
    // We need to produce the DTB from the DTS before doing anything to produce guest_images
    vm_images.step.dependOn(dtb_install_step);

    const kernel_image_arg = b.fmt("-DGUEST_KERNEL_IMAGE_PATH=\"{s}\"", .{ b.fmt("board/{s}/{s}/linux", .{ @tagName(microkit_board), vm_name }) });
    const initrd_image_arg = b.fmt("-DGUEST_INITRD_IMAGE_PATH=\"{s}\"", .{ b.getInstallPath(.prefix, rootfs_name) });
    const dtb_image_arg = b.fmt("-DGUEST_DTB_IMAGE_PATH=\"{s}\"", .{ b.getInstallPath(.prefix, dtb_name) });
    vm_images.addCSourceFile(.{
        .file = libvmm_dep.path("tools/package_guest_images.S"),
        .flags = &.{
            kernel_image_arg,
            dtb_image_arg,
            initrd_image_arg,
            "-x",
            "assembler-with-cpp",
        }
    });

    vmm.addObject(vm_images);

    return vmm;
}

pub fn build(b: *Build) void {
    const optimize = b.standardOptimizeOption(.{});

    // Microkit SDK option
    const microkit_sdk_option = b.option([]const u8, "sdk", "Path to Microkit SDK");
    if (microkit_sdk_option == null) {
        std.log.err("Missing -Dsdk=/path/to/sdk argument being passed\n", .{});
        std.posix.exit(1);
    }
    const microkit_sdk = microkit_sdk_option.?;
    std.fs.cwd().access(microkit_sdk, .{}) catch |err| {
        switch (err) {
            error.FileNotFound => std.log.err("Path to SDK '{s}' does not exist\n", .{ microkit_sdk }),
            else => std.log.err("Could not acccess SDK path '{s}', error is {any}\n", .{ microkit_sdk, err })
        }
        std.posix.exit(1);
    };

    // Microkit config option
    const microkit_config = b.option(ConfigOptions, "config", "Microkit config to build for") orelse ConfigOptions.debug;

    // Microkit board option
    const microkit_board_option = b.option(MicrokitBoard, "board", "Microkit board to target");
    if (microkit_board_option == null) {
        std.log.err("Missing -Dboard=<BOARD> argument being passed\n", .{});
        std.posix.exit(1);
    }
    microkit_board = microkit_board_option.?;
    const microkit_board_dir = b.fmt("{s}/board/{s}/{s}", .{ microkit_sdk, @tagName(microkit_board), @tagName(microkit_config) });
    std.fs.cwd().access(microkit_board_dir, .{}) catch |err| {
        switch (err) {
            error.FileNotFound => std.log.err("Path to '{s}' does not exist\n", .{ microkit_board_dir }),
            else => std.log.err("Could not acccess path '{s}', error is {any}\n", .{ microkit_board_dir, err })
        }
        std.posix.exit(1);
    };

    // mkvirtdisk option
    const mkvirtdisk_option = switch (microkit_board) {
        .qemu_virt_aarch64 => b.option(bool, "mkvirtdisk", "Create a new virtual disk image") orelse false,
        else => false,
    };

    // libmicrokit
    const libmicrokit_path = b.fmt("{s}/lib/libmicrokit.a", .{ microkit_board_dir });
    const libmicrokit_linker_script_path = b.fmt("{s}/lib/microkit.ld", .{ microkit_board_dir });
    const libmicrokit_include_path = b.fmt("{s}/include", .{ microkit_board_dir });
    const microkit_tool = LazyPath{ .cwd_relative = b.fmt("{s}/bin/microkit", .{ microkit_sdk }) };
    libmicrokit = LazyPath{ .cwd_relative = libmicrokit_path };
    libmicrokit_linker_script = LazyPath{ .cwd_relative = libmicrokit_linker_script_path };
    libmicrokit_include = LazyPath{ .cwd_relative = libmicrokit_include_path };

    // Target
    const target = b.resolveTargetQuery(findTarget(microkit_board));

    // Dependencies
    libvmm_dep = b.dependency("libvmm", .{
        .target = target,
        .optimize = optimize,
        .libmicrokit = @as([]const u8, libmicrokit_path),
        .libmicrokit_include = @as([]const u8, libmicrokit_include_path),
        .libmicrokit_linker_script = @as([]const u8, libmicrokit_linker_script_path),
        .microkit_board = @as([]const u8, @tagName(microkit_board)),
        .arm_vgic_version = @as(usize, 2),
        .blk_config_include = @as([]const u8, "include"),
    });
    sddf_dep = b.dependency("sddf", .{
        .target = target,
        .optimize = optimize,
        .libmicrokit = @as([]const u8, libmicrokit_path),
        .libmicrokit_include = @as([]const u8, libmicrokit_include_path),
        .libmicrokit_linker_script = @as([]const u8, libmicrokit_linker_script_path),
        .blk_config_include = @as([]const u8, "include"),
        .serial_config_include = @as([]const u8, "include")
    });

    // Blk driver VMM artifact
    const blk_driver_src_file = b.path("blk_driver_vmm.c");
    // Blk driver VM: Get DTB
    const blk_driver_vm_dts_base = b.path(b.fmt("board/{s}/blk_driver_vm/dts/linux.dts", .{ @tagName(microkit_board) }));
    const blk_driver_vm_dts_overlays = &[_]LazyPath{
        b.path(b.fmt("board/{s}/blk_driver_vm/dts/overlays/init.dts", .{ @tagName(microkit_board) })),
        b.path(b.fmt("board/{s}/blk_driver_vm/dts/overlays/io.dts", .{ @tagName(microkit_board) })),
        b.path(b.fmt("board/{s}/blk_driver_vm/dts/overlays/disable.dts", .{ @tagName(microkit_board) })),
    };
    // Blk driver VM: Pack rootfs
    const uio_driver_blk = libvmm_dep.artifact("uio_blk_driver");
    const blk_driver_vm_init = libvmm_dep.path(b.fmt("tools/linux/blk/board/{s}/blk_driver_init", .{ @tagName(microkit_board) }));
    const blk_driver_vm_rootfs_base = b.path(b.fmt("board/{s}/blk_driver_vm/rootfs.cpio.gz", .{ @tagName(microkit_board) }));
    const blk_driver_vmm = addVmm(
        b,
        "blk_driver_vm",
        "blk_driver_vmm.elf",
        blk_driver_src_file,
        blk_driver_vm_rootfs_base,
        &.{uio_driver_blk.getEmittedBin()},
        &.{blk_driver_vm_init},
        blk_driver_vm_dts_base,
        blk_driver_vm_dts_overlays,
        target,
        optimize
    );
    b.installArtifact(blk_driver_vmm);

    // Client VMM artifact
    const client_src_file = b.path("client_vmm.c");
    // Client VM: Get DTB
    const client_vm_dts_base = b.path(b.fmt("board/{s}/client_vm/dts/linux.dts", .{ @tagName(microkit_board) }));
    const client_vm_dts_overlays = &[_]LazyPath{
        b.path(b.fmt("board/{s}/client_vm/dts/overlays/init.dts", .{ @tagName(microkit_board) })),
        b.path(b.fmt("board/{s}/client_vm/dts/overlays/virtio.dts", .{ @tagName(microkit_board) })),
        b.path(b.fmt("board/{s}/client_vm/dts/overlays/disable.dts", .{ @tagName(microkit_board) })),
    };
    // Client VM: Pack rootfs
    const client_vm_init = libvmm_dep.path("tools/linux/blk/blk_client_init");
    const client_vm_rootfs_base = b.path(b.fmt("board/{s}/client_vm/rootfs.cpio.gz", .{ @tagName(microkit_board) }));
    const client_vmm = addVmm(
        b,
        "client_vm",
        "client_vmm.elf",
        client_src_file,
        client_vm_rootfs_base,
        &.{},
        &.{client_vm_init},
        client_vm_dts_base,
        client_vm_dts_overlays,
        target,
        optimize
    );
    b.installArtifact(client_vmm);

    // Get all of our sDDF artefacts
    // UART driver
    const uart_driver = switch (microkit_board) {
        .qemu_virt_aarch64 => sddf_dep.artifact("driver_uart_arm.elf"),
        .odroidc4 => sddf_dep.artifact("driver_uart_meson.elf"),
    };
    // The system description expects a different name for the UART driver ELF, so we add this extra
    // install step.
    const uart_driver_install = b.addInstallArtifact(uart_driver, .{ .dest_sub_path = "uart_driver.elf" });
    b.getInstallStep().dependOn(&uart_driver_install.step);
    // Serial TX virtualiser
    const serial_virt_tx = sddf_dep.artifact("serial_virt_tx.elf");
    b.installArtifact(serial_virt_tx);
    // Serial RX virtualiser
    const serial_virt_rx = sddf_dep.artifact("serial_virt_rx.elf");
    b.installArtifact(serial_virt_rx);
    // Block virtualiser
    const blk_virt = sddf_dep.artifact("blk_virt.elf");
    b.installArtifact(blk_virt);

    // Microkit tool command
    const system_description_path = LazyPath{ .cwd_relative = b.fmt("board/{s}/virtio.system", .{ @tagName(microkit_board) }) };
    const final_image_dest = b.getInstallPath(.bin, "./loader.img");
    const microkit_tool_cmd = b.addSystemCommand(&[_][]const u8{
        microkit_tool.getPath(b),
    });
    microkit_tool_cmd.addFileArg(system_description_path);
    microkit_tool_cmd.addArg("--search-path");
    microkit_tool_cmd.addArg(b.getInstallPath(.bin, ""));
    microkit_tool_cmd.addArg("--board");
    microkit_tool_cmd.addArg(@tagName(microkit_board));
    microkit_tool_cmd.addArg("--config");
    microkit_tool_cmd.addArg(@tagName(microkit_config));
    microkit_tool_cmd.addArg("-o");
    microkit_tool_cmd.addArg(final_image_dest);
    microkit_tool_cmd.addArg("-r");
    microkit_tool_cmd.addArg(b.getInstallPath(.prefix, "./report.txt"));
    microkit_tool_cmd.step.dependOn(b.getInstallStep());
    // Add the "microkit" step, and make it the default step when we execute `zig build`>
    const microkit_step = b.step("microkit", "Compile and build the final bootable image");
    microkit_step.dependOn(&microkit_tool_cmd.step);
    b.default_step = microkit_step;

    // Make virtual disk
    const virtdisk_name = "storage.img";
    var virtdisk: LazyPath = undefined;
    // Create virtual disk if it doesn't exist or if the user wants to create a new one
    const made_virtdisk = blk: {
        if (mkvirtdisk_option or !fileExists(b.getInstallPath(.prefix, virtdisk_name))) {
            // Depends on the number of clients, right now we always have two clients.
            const num_part = 2;
            // Configurable for the user
            // Default to 512 bytes
            const logical_size = b.option(usize, "virtdisk_logical_size", "Logical size for the virtual disk") orelse 512;
            // Default to 2MB
            const capacity = b.option(usize, "virtdisk_capacity", "Capacity of the virtual disk (in bytes)") orelse 2 * 1024 * 1024;
            _, virtdisk = libvmm_mod.mkvirtdisk(b, num_part, logical_size, capacity);
            break :blk true;
        } else {
            break :blk false;
        }
    };

    // This is setting up a `qemu` command for running the system using QEMU,
    // which we only want to do when we have a board that we can actually simulate.
    const loader_arg = b.fmt("loader,file={s},addr=0x70000000,cpu-num=0", .{ final_image_dest });
    if (microkit_board == .qemu_virt_aarch64) {
        const qemu_cmd = b.addSystemCommand(&[_][]const u8{
            "qemu-system-aarch64",
            "-machine",
            "virt,virtualization=on,highmem=off,secure=off",
            "-cpu",
            "cortex-a53",
            "-serial",
            "mon:stdio",
            "-device",
            loader_arg,
            "-m",
            "2G",
            "-nographic",
            "-drive",
            b.fmt("file={s},format=raw,if=none,id=drive0", .{b.getInstallPath(.prefix, virtdisk_name)}),
			"-device",
            "virtio-blk-device,drive=drive0,id=virtblk0,num-queues=1",
        });
        qemu_cmd.step.dependOn(microkit_step);
        if (made_virtdisk) {
            qemu_cmd.step.dependOn(&b.addInstallFileWithDir(virtdisk, .prefix, virtdisk_name).step);
        }
        const simulate_step = b.step("qemu", "Simulate the image using QEMU");
        simulate_step.dependOn(&qemu_cmd.step);
    }
}
