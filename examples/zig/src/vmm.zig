// Copyright 2023, UNSW
// SPDX-License-Identifier: BSD-2-Clause

const std = @import("std");
const c = @cImport({
    @cInclude("libvmm/virq.h");
    @cInclude("libvmm/guest.h");
    @cInclude("libvmm/arch/aarch64/linux.h");
});
const microkit = @import("libmicrokit.zig");

// In this example we only have one virtual CPU
const GUEST_BOOT_VCPU_ID = 0;
// There are the hard-coded addresses that both the VMM and Linux guest need
// to be aware of. For example the address of the DTB and initial RAM disk are
// passed to Linux when booting it.
const GUEST_RAM_VADDR: usize = 0x40000000;
const GUEST_DTB_VADDR: usize = 0x4f000000;
const GUEST_INIT_RAM_DISK_VADDR: usize = 0x4d700000;
const GUEST_RAM_SIZE: usize = 0x10000000;

// Below we make use of Zig's '@embedFile' builtin functions to easily include
// the arficats/images that the VMM needs. You will notice these aren't based
// on a relative or absolute path, this is because the build script enables us
// to find the files just based on the name by adding each one as a 'module'.

// Data for the guest's kernel image. This may look a bit complicated but
// what it's doing is making sure that the embedded file (which is just an array
// of bytes) is aligned to the alignment of the Linux image header in order to
// avoid UB when we cast the array of bytes into a struct for the Linux image
// hader.
const guest_kernel_image = blk: {
    const arr align(@alignOf(c.linux_image_header)) = @embedFile("linux").*;
    break :blk &arr;
};
// Data for the device tree to be passed to the kernel.
const guest_dtb_image = @embedFile("dtb");
// Data for the initial RAM disk to be passed to the kernel.
const guest_initrd_image = @embedFile("rootfs");

// In Zig the standard library comes with printf-like functionality with the
// ability to provide your own function to ouput the characters. This is
// extremely useful for us! Without changing the standard library or bringing
// in a 3rd party library, in a couple of lines of code we can get formatted
// printing functionality that ends up calling to microkit_dbg_puts which then
// outputs to the platform's serial connection.
//
// In a production system you might want to instead hook up the logger to a
// user-level UART driver, but for the example we only need logging for debug
// mode.
const log = struct {
    const Writer = std.io.Writer(u32, error{}, debug_uart_put_str);
    const debug_uart = Writer { .context = 0 };

    fn debug_uart_put_str(_: u32, str: []const u8) !usize {
        for (str) |ch| {
            microkit.microkit_dbg_putc(ch);
        }
        return str.len;
    }

    pub fn info(comptime fmt: []const u8, args: anytype) void {
        debug_uart.print("VMM|INFO: " ++ fmt ++ "\n", args) catch {};
    }

    pub fn err(comptime fmt: []const u8, args: anytype) void {
        debug_uart.print("VMM|ERROR: " ++ fmt ++ "\n", args) catch {};
    }
};

const SERIAL_IRQ_CH: microkit.microkit_channel = 1;
const SERIAL_IRQ: i32 = 33;

fn serial_ack(_: usize, _: c_int, _: ?*anyopaque) callconv(.C) void {
    // Nothing else needs to be done other than acking the IRQ.
    microkit.microkit_irq_ack(SERIAL_IRQ_CH);
}

export fn init() callconv(.C) void {
    // Initialise the VMM, the VCPU(s), and start the guest
    log.info("starting \"{s}\"", .{ microkit.microkit_name });
    // Place all the binaries in the right locations before starting the guest
    const kernel_pc = c.linux_setup_images(
                GUEST_RAM_VADDR,
                @intFromPtr(guest_kernel_image),
                guest_kernel_image.len,
                @intFromPtr(guest_dtb_image),
                GUEST_DTB_VADDR,
                guest_dtb_image.len,
                @intFromPtr(guest_initrd_image),
                GUEST_INIT_RAM_DISK_VADDR,
                guest_initrd_image.len
            );
    if (kernel_pc == 0) {
        log.err("Failed to initialise guest images\n", .{});
        return;
    }
    // Initialise the virtual interrupt controller
    if (!c.virq_controller_init()) {
        log.err("Failed to initialise virtual interrupt controller\n", .{});
        return;
    }
    // Register the interrupt for the UART with the virtual interrupt controller
    if (!c.virq_register(GUEST_BOOT_VCPU_ID, SERIAL_IRQ, &serial_ack, null)) {
        log.err("Failed to register serial IRQ\n", .{});
        return;
    }
    // Just in case there is already an interrupt from the UART available to
    // handle, we ack it here.
    microkit.microkit_irq_ack(SERIAL_IRQ_CH);
    // Finally we can start the guest
    if (!c.guest_start(kernel_pc, GUEST_DTB_VADDR, GUEST_INIT_RAM_DISK_VADDR)) {
        log.err("Failed to start guest\n", .{});
        return;
    }
}

export fn notified(ch: microkit.microkit_channel) callconv(.C) void {
    switch (ch) {
        SERIAL_IRQ_CH => {
            const success = c.virq_inject(SERIAL_IRQ);
            if (!success) {
                log.err("IRQ {x} dropped\n", .{ SERIAL_IRQ });
            }
        },
        else => log.err("Unexpected channel, ch: {x}\n", .{ ch })
    }
}

extern fn fault_handle(id: microkit.microkit_child, msginfo: microkit.microkit_msginfo) callconv(.C) bool;

export fn fault(id: microkit.microkit_child, msginfo: microkit.microkit_msginfo, msginfo_reply: *microkit.microkit_msginfo) callconv(.C) bool {
    if (fault_handle(id, msginfo)) {
        // Now that we have handled the fault, we reply to it so that the guest can resume execution.
        msginfo_reply.* = microkit.microkit_msginfo_new(0, 0);
        return true;
    } else {
        log.err("Failed to handle fault\n", .{});
        return false;
    }
}
