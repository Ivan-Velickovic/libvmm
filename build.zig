// Copyright 2024, UNSW
// SPDX-License-Identifier: BSD-2-Clause

const std = @import("std");
const LazyPath = std.Build.LazyPath;

const src = [_][]const u8{
    "src/guest.c",
    "src/util/util.c",
    "src/util/printf.c",
    "src/virtio/mmio.c",
    "src/virtio/block.c",
    "src/virtio/console.c",
    "src/virtio/net.c",
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

const src_riscv64 = [_][]const u8{
    "src/arch/riscv/fault.c",
    "src/arch/riscv/linux.c",
    "src/arch/riscv/tcb.c",
    "src/arch/riscv/vcpu.c",
    "src/arch/riscv/virq.c",
};

pub fn build(b: *std.Build) void {
    const optimize = b.standardOptimizeOption(.{});
    const target = b.standardTargetOptions(.{});

    const libmicrokit_opt = b.option([]const u8, "libmicrokit", "Path to libmicrokit.a") orelse null;
    const libmicrokit_include_opt = b.option([]const u8, "libmicrokit_include", "Path to the libmicrokit include directory") orelse null;
    const libmicrokit_linker_script_opt = b.option([]const u8, "libmicrokit_linker_script", "Path to the libmicrokit linker script") orelse null;

    // Default to vGIC version 2
    const arm_vgic_version = b.option(usize, "arm_vgic_version", "ARM vGIC version to emulate") orelse null;

    const libvmm = b.addStaticLibrary(.{
        .name = "vmm",
        .target = target,
        .optimize = optimize,
    });

    const sddf = b.dependency("sddf", .{
        .target = target,
        .optimize = optimize,
        .libmicrokit = @as([]const u8, libmicrokit_opt.?),
        .libmicrokit_include = @as([]const u8, libmicrokit_include_opt.?),
        .libmicrokit_linker_script = @as([]const u8, libmicrokit_linker_script_opt.?),
    });

    const src_files = switch (target.result.cpu.arch) {
        .aarch64 => blk: {
            const vgic_src = switch (arm_vgic_version.?) {
                2 => src_aarch64_vgic_v2,
                3 => src_aarch64_vgic_v3,
                else => @panic("Unsupported vGIC version given"),
            };

            break :blk &(src ++ src_aarch64 ++ vgic_src);
        },
        .riscv64 => &(src ++ src_riscv64),
        else => {
            std.log.err("Unsupported libvmm architecture given '{s}'", .{ @tagName(target.result.cpu.arch) });
            std.posix.exit(1);
        }
    };
    libvmm.addCSourceFiles(.{
        .files = src_files,
        .flags = &.{
            "-Wall",
            "-Werror",
            "-Wno-unused-function",
            "-mstrict-align",
            "-fno-sanitize=undefined", // @ivanv: ideally we wouldn't have to turn off UBSAN
        }
    });

    libvmm.addIncludePath(b.path("include"));
    libvmm.addIncludePath(sddf.path("include"));
    libvmm.addIncludePath(.{ .cwd_relative = libmicrokit_include_opt.? });

    libvmm.installHeadersDirectory(b.path("include/libvmm"), "libvmm", .{});
    libvmm.installHeadersDirectory(sddf.path("include/sddf"), "sddf", .{});

    libvmm.linkLibrary(sddf.artifact("util"));
    libvmm.linkLibrary(sddf.artifact("util_putchar_debug"));

    b.installArtifact(libvmm);
}
