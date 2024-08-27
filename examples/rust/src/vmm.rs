// Copyright 2024, UNSW
// SPDX-License-Identifier: BSD-2-Clause

#![no_std]
#![no_main]
#![feature(never_type)]

use core::{include_bytes};
use core::ffi::{c_void};

use sel4_microkit::{protection_domain, MessageInfo, Channel, Child, Handler, debug_println};

const GUEST_RAM_VADDR: usize = 0x40000000;
const GUEST_DTB_VADDR: usize = 0x4f000000;
const GUEST_INIT_RAM_DISK_VADDR: usize = 0x4d700000;
const GUEST_BOOT_VCPU_ID: usize = 0;

/// On the QEMU virt AArch64 platform the UART we are using has an IRQ number of 33.
const UART_IRQ: usize = 33;
/// The VMM is expecting the IRQ to be delivered with the channel ID of 1.
const UART_CH: Channel = Channel::new(1);

// This is arguably the hardest part of writing the VMM in Rust, it is the act of
// connecting the Rust code to libvmm which is written in C. Thankfully, it is not
// too bad for us to manually create the bindings, given that there are only a few
// VMM calls in order to setup a basic Linux guest.
//
// However, in the future it might not be realistic to do this, especially with other
// services of the VMM such as virtIO. Ultimately if you are writing a VMM in Rust
// then you may want to look into bindgen for automatically creating these bindings,
// or even creating your own "Rust-like" bindings for libvmm to make it nicer to use
// (or closer to feeling like a Rust crate), or both!
#[link(name = "vmm", kind = "static")]
#[link(name = "microkit", kind = "static")]
extern "C" {
    fn linux_setup_images(ram_start: usize,
                          kernel: usize, kernel_size: usize,
                          dtb_src: usize, dtb_dest: usize, dtb_size: usize,
                          initrd_src: usize, initrd_dest: usize, initrd_size: usize) -> usize;
    fn virq_controller_init() -> bool;
    fn virq_register(vcpu_id: usize, irq: i32, ack_fn: extern fn(usize, i32, *const c_void), ack_data: *const c_void) -> bool;
    fn virq_inject(irq: i32) -> bool;
    fn guest_start(kernel_pc: usize, dtb: usize, initrd: usize) -> bool;
    fn fault_handle(vcpu_id: usize, msginfo: MessageInfo) -> bool;
}

extern "C" fn uart_irq_ack(_: usize, _: i32, _: *const c_void) {
    match UART_CH.irq_ack() {
        // Do nothing if there's no problem
        Ok(()) => {}
        Err(_e) => {
            debug_println!("VMM|ERROR: received ack from guest, but could not ack UART IRQ channel: {_e}");
        }
    }
}

#[protection_domain]
fn init() -> VmmHandler {
    debug_println!("VMM|INFO: starting Rust VMM");
    // Thankfully Rust comes with a simple macro that allows us to package
    // binaries locally and placing them in the final binary that we load
    // onto our platform, in this case the QEMU virt AArch64 board.
    // @ivanv: the way this include works right now is undefined behaviour!
    let linux = include_bytes!(concat!(env!("IMAGE_DIR"), "/linux"));
    let dtb = include_bytes!(concat!(env!("BUILD_DIR"), "/linux.dtb"));
    let initrd = include_bytes!(concat!(env!("IMAGE_DIR"), "/rootfs.cpio.gz"));
    // libvmm does not understand slices like Rust, so we have to
    // turn this slices of u8 into raw addresses.
    let linux_addr = linux.as_ptr() as usize;
    let dtb_addr = dtb.as_ptr() as usize;
    let initrd_addr = initrd.as_ptr() as usize;

    unsafe {
        let guest_pc = linux_setup_images(GUEST_RAM_VADDR,
                                            linux_addr, linux.len(),
                                            dtb_addr, GUEST_DTB_VADDR, dtb.len(),
                                            initrd_addr, GUEST_INIT_RAM_DISK_VADDR, initrd.len()
                                         );
        let success = virq_controller_init();
        assert!(success);
        let success = virq_register(GUEST_BOOT_VCPU_ID, UART_IRQ as i32, uart_irq_ack, core::ptr::null());
        assert!(success);
        match UART_CH.irq_ack() {
            Ok(()) => {}
            Err(_e) => {
                debug_println!("VMM|ERROR: could not ack UART IRQ channel: {_e}");
            }
        }
        guest_start(guest_pc, GUEST_DTB_VADDR, GUEST_INIT_RAM_DISK_VADDR);
    }

    VmmHandler {}
}

struct VmmHandler {}

impl Handler for VmmHandler {
    type Error = !;

    fn notified(&mut self, channel: Channel) -> Result<(), Self::Error> {
        match channel {
            UART_CH => {
                unsafe {
                    let success = virq_inject(UART_IRQ as i32);
                    if !success {
                        debug_println!("VMM|ERROR: could not inject UART IRQ");
                    }
                }
            }
            _ => unreachable!()
        }
        Ok(())
    }

    fn fault(&mut self, id: Child, msg_info: MessageInfo) -> Result<Option<MessageInfo>, Self::Error> {
        unsafe {
            if fault_handle(id.index(), msg_info) {
                Ok(Some(MessageInfo::new(0, 0)))
            } else {
                unreachable!()
            }
        }
    }
}
