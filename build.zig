// Copyright 2024, UNSW
// SPDX-License-Identifier: BSD-2-Clause

const std = @import("std");
const LazyPath = std.Build.LazyPath;

const src = [_][]const u8{
    "src/guest.c",
    "src/util/util.c",
    "src/util/printf.c",
    "src/virtio/block.c",
    "src/virtio/console.c",
    "src/virtio/mmio.c",
    "src/virtio/sound.c",
};

const src_aarch64_vgic_v2 = [_][]const u8{
    "src/arch/aarch64/vgic/vgic_v2.c",
};

const src_aarch64_vgic_v3 = [_][]const u8{
    "src/arch/aarch64/vgic/vgic_v3.c",
};

const src_aarch64 = [_][]const u8{
    "src/arch/aarch64/vgic/vgic.c",
    "src/arch/aarch64/fault.c",
    "src/arch/aarch64/psci.c",
    "src/arch/aarch64/smc.c",
    "src/arch/aarch64/virq.c",
    "src/arch/aarch64/linux.c",
    "src/arch/aarch64/tcb.c",
    "src/arch/aarch64/vcpu.c",
};

pub fn dtscat(
    b: *std.Build,
    base: LazyPath,
    overlays: []const LazyPath
) std.meta.Tuple(&.{*std.Build.Step.Run, LazyPath}) {
    const dtscat_tool = b.dependencyFromBuildZig(@This(), .{}).path("tools/dtscat");

    const cmd = std.Build.Step.Run.create(b, "run dtscat");
    cmd.addFileArg(dtscat_tool);
    cmd.addFileArg(base);
    for (overlays) |overlay| {
        cmd.addFileArg(overlay);
    }
    const output = cmd.captureStdOut();
    return .{cmd, output};
}

pub fn compileDts(
    b: *std.Build,
    dts: LazyPath
) std.meta.Tuple(&.{*std.Build.Step.Run, LazyPath}) {
    const cmd = b.addSystemCommand(&[_][]const u8{
        "dtc", "-q", "-I", "dts", "-O", "dtb"
    });
    cmd.addFileArg(dts);
    const output = cmd.captureStdOut();
    return .{cmd, output};
}

pub fn packrootfs(
    b: *std.Build,
    rootfs_base: LazyPath,
    rootfs_tmp: []const u8,
    rootfs_name: []const u8,
    home_files: []const LazyPath,
    startup_files: []const LazyPath,
) std.meta.Tuple(&.{*std.Build.Step.Run, LazyPath})
{
    const packrootfs_tool = b.dependencyFromBuildZig(@This(), .{}).path("tools/packrootfs");
    // TODO: Investigate why WF is not working
    // const rootfs_wf = b.addWriteFiles();
    // const output = rootfs_wf.getDirectory().path(b, rootfs_name);
    // const tmp = rootfs_wf.getDirectory().path(b, "tmp");
    const cmd = std.Build.Step.Run.create(b, "run packrootfs");
    cmd.addFileArg(packrootfs_tool);
    cmd.addFileArg(rootfs_base);
    cmd.addArg(rootfs_tmp);
    cmd.addArg("-o");
    const output = cmd.addOutputFileArg(rootfs_name);
    cmd.addArg("--home");
    for (home_files) |file| {
        cmd.addFileArg(file);
    }
    cmd.addArg("--startup");
    for (startup_files) |file| {
        cmd.addFileArg(file);
    }
    return .{cmd, output};
}

pub fn mkvirtdisk(
    b: *std.Build,
    num_part: usize,
    logical_size: usize,
    blk_mem: usize,
) std.meta.Tuple(&.{*std.Build.Step.Run, LazyPath}) {
    const this_dep = b.dependencyFromBuildZig(@This(), .{});
    const mkvirtdisk_tool = this_dep.path("tools/mkvirtdisk");

    const num_part_str = b.fmt("{d}", .{ num_part });
    const logical_size_str = b.fmt("{d}", .{ logical_size });
    const blk_mem_str = b.fmt("{d}", .{ blk_mem });
    const cmd = std.Build.Step.Run.create(b, "run mkvirtdisk");
    cmd.addFileArg(mkvirtdisk_tool);
    // TODO: Investigate another way to do this, don't use /tmp?
    const virtdisk = cmd.addOutputFileArg("/tmp/virtdisk");
    cmd.addArg(num_part_str);
    cmd.addArg(logical_size_str);
    cmd.addArg(blk_mem_str);
    return .{cmd, virtdisk};
}

pub fn build(b: *std.Build) void {
    const optimize = b.standardOptimizeOption(.{});
    const target = b.standardTargetOptions(.{});

    const libmicrokit_opt = b.option([]const u8, "libmicrokit", "Path to libmicrokit.a") orelse "";
    const libmicrokit_include_opt = b.option([]const u8, "libmicrokit_include", "Path to the libmicrokit include directory") orelse "";
    const libmicrokit_linker_script_opt = b.option([]const u8, "libmicrokit_linker_script", "Path to the libmicrokit linker script") orelse "";
    const microkit_board_opt = b.option([]const u8, "microkit_board", "Name of Microkit board") orelse "";
    // Default to vGIC version 2
    const arm_vgic_version = b.option(usize, "arm_vgic_version", "ARM vGIC version to emulate") orelse 2;
    const blk_config_include_opt = b.option([]const u8, "blk_config_include", "Include path to block config header") orelse "";

    const libmicrokit_include = libmicrokit_include_opt;
    const blk_config_include = std.Build.LazyPath{ .cwd_relative = blk_config_include_opt };

    const sddf_dep = b.dependency("sddf", .{
        .target = target,
        .optimize = optimize,
        .libmicrokit = @as([]const u8, libmicrokit_opt),
        .libmicrokit_include = @as([]const u8, libmicrokit_include),
        .libmicrokit_linker_script = @as([]const u8, libmicrokit_linker_script_opt),
        .blk_config_include = @as([]const u8, blk_config_include_opt),
    });

    const libvmm = b.addStaticLibrary(.{
        .name = "vmm",
        .target = target,
        .optimize = optimize,
    });

    const src_arch = switch (target.result.cpu.arch) {
        .aarch64 => blk: {
            const vgic_src = switch (arm_vgic_version) {
                2 => src_aarch64_vgic_v2,
                3 => src_aarch64_vgic_v3,
                else => @panic("Unsupported vGIC version given"),
            };

            break :blk &(src_aarch64 ++ vgic_src);
        },
        else => {
            @panic(b.fmt("Unsupported libvmm architecture given '{s}'", .{ @tagName(target.result.cpu.arch) }));
        }
    };

    const libvmm_flags = &.{
            "-Wall",
            "-Werror",
            "-Wno-unused-function",
            "-mstrict-align",
            "-fno-sanitize=undefined", // @ivanv: ideally we wouldn't have to turn off UBSAN
            b.fmt("-DBOARD_{s}", .{ microkit_board_opt }) // @ivanv: shouldn't be needed as the library should not depend on the board
    };
    libvmm.addCSourceFiles(.{
        .files = &src,
        .flags = libvmm_flags,
    });
    libvmm.addCSourceFiles(.{
        .files = src_arch,
        .flags = libvmm_flags,
    });

    libvmm.addIncludePath(b.path("include"));
    libvmm.addIncludePath(sddf_dep.path("include"));
    libvmm.addIncludePath(.{ .cwd_relative = libmicrokit_include });
    libvmm.linkLibrary(sddf_dep.artifact("util"));
    libvmm.linkLibrary(sddf_dep.artifact("util_putchar_debug"));

    libvmm.installHeadersDirectory(b.path("include/libvmm"), "libvmm", .{});

    b.installArtifact(libvmm);

    var target_userlevel_query = target.query;
    target_userlevel_query.os_tag = .linux;
    target_userlevel_query.abi = .musl;
    const target_userlevel = b.resolveTargetQuery(target_userlevel_query);

    const uio_driver_blk = b.addExecutable(.{
        .name = "uio_blk_driver",
        .target = target_userlevel,
        .optimize = optimize,
        .strip = false,
    });
    uio_driver_blk.addCSourceFiles(.{
        .files = &.{
            "tools/linux/uio_drivers/blk/blk.c",
            "tools/linux/uio/libuio.c",
        },
        .flags = &.{
            "-Wall",
            "-Werror",
            "-Wno-unused-function",
        }
    });
    uio_driver_blk.linkLibC();
    uio_driver_blk.addIncludePath(b.path("tools/linux/include"));
    uio_driver_blk.addIncludePath(blk_config_include);
    uio_driver_blk.addIncludePath(sddf_dep.path("include"));
    b.installArtifact(uio_driver_blk);
}
