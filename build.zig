// Copyright 2024, UNSW
// SPDX-License-Identifier: BSD-2-Clause

const std = @import("std");

const src = [_][]const u8{
    "src/guest.c",
    "src/util/util.c",
    "src/util/printf.c",
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
    "src/arch/riscv/plic.c",
};

pub fn build(b: *std.Build) void {
    const optimize = b.standardOptimizeOption(.{});
    const target = b.standardTargetOptions(.{});

    const libmicrokit_include_opt = b.option([]const u8, "libmicrokit_include", "Path to the libmicrokit include directory") orelse null;

    // Default to vGIC version 2
    const arm_vgic_version = b.option(usize, "arm_vgic_version", "ARM vGIC version to emulate") orelse null;

    const libmicrokit_include = libmicrokit_include_opt.?;

    const libvmm = b.addStaticLibrary(.{
        .name = "vmm",
        .target = target,
        .optimize = optimize,
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
    libvmm.addIncludePath(.{ .cwd_relative = libmicrokit_include });

    libvmm.installHeadersDirectory(b.path("include/libvmm"), "libvmm", .{});

    b.installArtifact(libvmm);
}
