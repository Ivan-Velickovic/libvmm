// Copyright 2024, UNSW
// SPDX-License-Identifier: BSD-2-Clause

const std = @import("std");

const MicrokitBoard = enum {
    qemu_virt_aarch64,
    odroidc4,
    maaxboard,
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
        },
    },
    .{
        .board = MicrokitBoard.odroidc4,
        .zig_target = std.Target.Query{
            .cpu_arch = .aarch64,
            .cpu_model = .{ .explicit = &std.Target.arm.cpu.cortex_a55 },
            .os_tag = .freestanding,
            .abi = .none,
        },
    },
    .{
        .board = MicrokitBoard.maaxboard,
        .zig_target = std.Target.Query{
            .cpu_arch = .aarch64,
            .cpu_model = .{ .explicit = &std.Target.arm.cpu.cortex_a53 },
            .os_tag = .freestanding,
            .abi = .none,
        },
    },
};

fn findTarget(board: MicrokitBoard) std.Target.Query {
    for (targets) |target| {
        if (board == target.board) {
            return target.zig_target;
        }
    }

    std.log.err("Board '{}' is not supported\n", .{ board });
    std.posix.exit(1);
}

const ConfigOptions = enum {
    debug,
    release,
    benchmark
};

pub fn build(b: *std.Build) void {
    const optimize = b.standardOptimizeOption(.{});

    // Getting the path to the Microkit SDK before doing anything else
    const microkit_sdk_arg = b.option([]const u8, "sdk", "Path to Microkit SDK");
    if (microkit_sdk_arg == null) {
        std.log.err("Missing -Dsdk=/path/to/sdk argument being passed\n", .{});
        std.posix.exit(1);
    }
    const microkit_sdk = microkit_sdk_arg.?;

    const microkit_config_option = b.option(ConfigOptions, "config", "Microkit config to build for") orelse ConfigOptions.debug;
    const microkit_config = @tagName(microkit_config_option);

    // Get the Microkit SDK board we want to target
    const microkit_board_option = b.option(MicrokitBoard, "board", "Microkit board to target");

    if (microkit_board_option == null) {
        std.log.err("Missing -Dboard=<BOARD> argument being passed\n", .{});
        std.posix.exit(1);
    }
    const target = b.resolveTargetQuery(findTarget(microkit_board_option.?));
    const microkit_board = @tagName(microkit_board_option.?);

    // Since we are relying on Zig to produce the final ELF, it needs to do the
    // linking step as well.
    const microkit_board_dir = b.fmt("{s}/board/{s}/{s}", .{ microkit_sdk, microkit_board, microkit_config });
    const microkit_tool = b.fmt("{s}/bin/microkit", .{ microkit_sdk });
    const libmicrokit = b.fmt("{s}/lib/libmicrokit.a", .{ microkit_board_dir });
    const libmicrokit_linker_script = b.fmt("{s}/lib/microkit.ld", .{ microkit_board_dir });
    const libmicrokit_include = b.fmt("{s}/include", .{ microkit_board_dir });

    const arm_vgic_version: usize = switch (microkit_board_option.?) {
        .qemu_virt_aarch64, .odroidc4 => 2,
        .maaxboard => 3,
    };

    const libvmm_dep = b.dependency("libvmm", .{
        .target = target,
        .optimize = optimize,
        .libmicrokit_include = @as([]const u8, libmicrokit_include),
        .arm_vgic_version = arm_vgic_version,
    });
    const libvmm = libvmm_dep.artifact("vmm");

    const exe = b.addExecutable(.{
        .name = "vmm.elf",
        .target = target,
        .optimize = optimize,
        // Microkit expects and requires the symbol table to exist in the ELF,
        // this means that even when building for release mode, we want to tell
        // Zig not to strip symbols from the binary.
        .strip = false,
    });

    const base_dts_path = b.fmt("board/{s}/linux.dts", .{ microkit_board });
    const overlay = b.fmt("board/{s}/overlay.dts", .{ microkit_board });
    const dts_cat_cmd = b.addSystemCommand(&[_][]const u8{
        "sh", "../../tools/dtscat", base_dts_path, overlay
    });
    const final_dts = dts_cat_cmd.captureStdOut();

    // For actually compiling the DTS into a DTB
    const dtc_cmd = b.addSystemCommand(&[_][]const u8{
        "dtc", "-q", "-I", "dts", "-O", "dtb"
    });
    dtc_cmd.addFileArg(.{ .cwd_relative = b.getInstallPath(.prefix, "final.dts") });
    dtc_cmd.step.dependOn(&b.addInstallFileWithDir(final_dts, .prefix, "final.dts").step);
    const dtb = dtc_cmd.captureStdOut();

    // Add microkit.h to be used by the API wrapper.
    exe.addIncludePath(.{ .cwd_relative = libmicrokit_include });
    // Add the static library that provides each protection domain's entry
    // point (`main()`), which runs the main handler loop.
    exe.addObjectFile(.{ .cwd_relative = libmicrokit });
    // Specify the linker script, this is necessary to set the ELF entry point address.
    exe.setLinkerScriptPath(.{ .cwd_relative = libmicrokit_linker_script });

    // Link the libvmm library, this will automatically add the libvmm headers as well.
    exe.linkLibrary(libvmm);

    exe.addCSourceFiles(.{
        .files = &.{"vmm.c"},
        .flags = &.{
            "-Wall",
            "-Werror",
            "-Wno-unused-function",
            "-mstrict-align",
            b.fmt("-DBOARD_{s}", .{ microkit_board })
        }
    });

    const guest_images = b.addObject(.{
        .name = "guest_images",
        .target = target,
        .optimize = optimize,
    });
    // We need to produce the DTB from the DTS before doing anything to produce guest_images
    guest_images.step.dependOn(&b.addInstallFileWithDir(dtb, .prefix, "linux.dtb").step);

    const linux_image_dep = b.lazyDependency(b.fmt("{s}_linux", .{ microkit_board }), .{});
    const initrd_image_dep = b.lazyDependency(b.fmt("{s}_initrd", .{ microkit_board }), .{});

    if (linux_image_dep) |linux_image| {
        guest_images.step.dependOn(&b.addInstallFileWithDir(linux_image.path("linux"), .prefix, "linux").step);
    }

    if (initrd_image_dep) |initrd_image| {
        guest_images.step.dependOn(&b.addInstallFileWithDir(initrd_image.path("rootfs.cpio.gz"), .prefix, "rootfs.cpio.gz").step);
    }

    const kernel_image_arg = b.fmt("-DGUEST_KERNEL_IMAGE_PATH=\"{s}\"", .{ b.getInstallPath(.prefix, "linux") });
    const initrd_image_arg = b.fmt("-DGUEST_INITRD_IMAGE_PATH=\"{s}\"", .{ b.getInstallPath(.prefix, "rootfs.cpio.gz") });
    const dtb_image_arg = b.fmt("-DGUEST_DTB_IMAGE_PATH=\"{s}\"", .{ b.getInstallPath(.prefix, "linux.dtb") });
    guest_images.addCSourceFile(.{
        .file = libvmm_dep.path("tools/package_guest_images.S"),
        .flags = &.{
            kernel_image_arg,
            dtb_image_arg,
            initrd_image_arg,
            "-x",
            "assembler-with-cpp",
        }
    });

    exe.addObject(guest_images);
    b.installArtifact(exe);

    const system_description_path = b.fmt("board/{s}/simple.system", .{ microkit_board });
    const final_image_dest = b.getInstallPath(.bin, "./loader.img");
    const microkit_tool_cmd = b.addSystemCommand(&[_][]const u8{
       microkit_tool,
       system_description_path,
       "--search-path",
       b.getInstallPath(.bin, ""),
       "--board",
       microkit_board,
       "--config",
       microkit_config,
       "-o",
       final_image_dest,
       "-r",
       b.getInstallPath(.prefix, "./report.txt")
    });
    microkit_tool_cmd.step.dependOn(b.getInstallStep());
    // Add the "microkit" step, and make it the default step when we execute `zig build`
    const microkit_step = b.step("microkit", "Compile and build the final bootable image");
    microkit_step.dependOn(&microkit_tool_cmd.step);
    b.default_step = microkit_step;

    // This is setting up a `qemu` command for running the system using QEMU,
    // which we only want to do when we have a board that we can actually simulate.
    const loader_arg = b.fmt("loader,file={s},addr=0x70000000,cpu-num=0", .{ final_image_dest });
    if (std.mem.eql(u8, microkit_board, "qemu_virt_aarch64")) {
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
        });
        qemu_cmd.step.dependOn(b.default_step);
        const simulate_step = b.step("qemu", "Simulate the image using QEMU");
        simulate_step.dependOn(&qemu_cmd.step);
    }
}
