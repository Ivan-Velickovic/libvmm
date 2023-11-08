/*
 * Copyright 2023, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stddef.h>
#include <stdint.h>
#include <microkit.h>
#include "util/util.h"
#include "arch/aarch64/vgic/vgic.h"
#include "arch/aarch64/linux.h"
#include "arch/aarch64/fault.h"
#include "guest.h"
#include "virq.h"
#include "tcb.h"
#include "vcpu.h"

/* FOR ODROIDC4 */
#define GUEST_RAM_SIZE 0x10000000
#define GUEST_DTB_VADDR 0x2f000000
#define GUEST_INIT_RAM_DISK_VADDR 0x2d700000

/* Data for the guest's kernel image. */
extern char _guest_kernel_image[];
extern char _guest_kernel_image_end[];
/* Data for the device tree to be passed to the kernel. */
extern char _guest_dtb_image[];
extern char _guest_dtb_image_end[];
/* Data for the initial RAM disk to be passed to the kernel. */
extern char _guest_initrd_image[];
extern char _guest_initrd_image_end[];
/* microkit will set this variable to the start of the guest RAM memory region. */
uintptr_t guest_ram_vaddr;

#define PASSTHROUGH_BLK_IRQ 222
#define PASSTHROUGH_BLK_ID 3
#define UIO_BLK_IRQ 50
#define VSWITCH_BLK 1

#define MAX_IRQ_CH 63
int passthrough_irq_map[MAX_IRQ_CH];

static void dummy_ack(size_t vcpu_id, int irq, void *cookie) {
    return;
}

static void passthrough_device_ack(size_t vcpu_id, int irq, void *cookie) {
    microkit_channel irq_ch = (microkit_channel)(int64_t)cookie;
    microkit_irq_ack(irq_ch);
}

static void register_passthrough_irq(int irq, microkit_channel irq_ch) {
    LOG_VMM("Register passthrough IRQ %d (channel: 0x%lx)\n", irq, irq_ch);
    assert(irq_ch < MAX_IRQ_CH);
    passthrough_irq_map[irq_ch] = irq;

    int err = virq_register(GUEST_VCPU_ID, irq, &passthrough_device_ack, (void *)(int64_t)irq_ch);
    if (!err) {
        LOG_VMM_ERR("Failed to register IRQ %d\n", irq);
        return;
    }
}

/* sDDF memory regions for virtio blk */
uintptr_t cmdq_avail;
uintptr_t cmdq_used;
uintptr_t cmdq_shm;
uintptr_t resp_avail;
uintptr_t resp_used;
uintptr_t resp_shm;

void init(void) {
    /* Initialise the VMM, the VCPU(s), and start the guest */
    LOG_VMM("starting \"%s\"\n", microkit_name);
    /* Place all the binaries in the right locations before starting the guest */
    size_t kernel_size = _guest_kernel_image_end - _guest_kernel_image;
    size_t dtb_size = _guest_dtb_image_end - _guest_dtb_image;
    size_t initrd_size = _guest_initrd_image_end - _guest_initrd_image;
    uintptr_t kernel_pc = linux_setup_images(guest_ram_vaddr,
                                      (uintptr_t) _guest_kernel_image,
                                      kernel_size,
                                      (uintptr_t) _guest_dtb_image,
                                      GUEST_DTB_VADDR,
                                      dtb_size,
                                      (uintptr_t) _guest_initrd_image,
                                      GUEST_INIT_RAM_DISK_VADDR,
                                      initrd_size
                                      );
    if (!kernel_pc) {
        LOG_VMM_ERR("Failed to initialise guest images\n");
        return;
    }
    /* Initialise the virtual GIC driver */
    bool success = virq_controller_init(GUEST_VCPU_ID);
    if (!success) {
        LOG_VMM_ERR("Failed to initialise emulated interrupt controller\n");
        return;
    }

    register_passthrough_irq(225, 1);
    register_passthrough_irq(222, 5);   // @tim: jade had as 2
    register_passthrough_irq(223, 3);
    register_passthrough_irq(232, 4);

    register_passthrough_irq(40, 2);   // @tim: jade had as 5
    register_passthrough_irq(35, 15);

    register_passthrough_irq(96, 6);
    register_passthrough_irq(192, 7);
    register_passthrough_irq(193, 8);
    register_passthrough_irq(194, 9);
    register_passthrough_irq(53, 10);
    register_passthrough_irq(228, 11);
    register_passthrough_irq(63, 12);
    register_passthrough_irq(62, 13);
    register_passthrough_irq(48, 16);
    register_passthrough_irq(89, 14);
    // @jade: this should not be necessary. Investigation required.
    register_passthrough_irq(5, 17);


    /* Register MMC passthrough */
    // register_passthrough_irq(PASSTHROUGH_BLK_IRQ, PASSTHROUGH_BLK_ID);

    /* Register UIO irq */
    // virq_register(GUEST_VCPU_ID, UIO_BLK_IRQ, &dummy_ack, NULL);

    // Silence unused sDDF variable warnings, lets just print them out for now
    // printf("cmdq_avail: 0x%lx\n", cmdq_avail);
    // printf("cmdq_used: 0x%lx\n", cmdq_used);
    // printf("cmdq_shm: 0x%lx\n", cmdq_shm);
    // printf("resp_avail: 0x%lx\n", resp_avail);
    // printf("resp_used: 0x%lx\n", resp_used);
    // printf("resp_shm: 0x%lx\n", resp_shm);

    /* Finally start the guest */
    guest_start(GUEST_VCPU_ID, kernel_pc, GUEST_DTB_VADDR, GUEST_INIT_RAM_DISK_VADDR);
}

void notified(microkit_channel ch) {
    if (ch == 1) {
        printf("SERIAL IRQ\n");
    }

    switch (ch) {
        case VSWITCH_BLK:
            // virq_inject(GUEST_VCPU_ID, UIO_BLK_IRQ);
            break;
        default:
            if (passthrough_irq_map[ch]) {
                bool success = virq_inject(GUEST_VCPU_ID, passthrough_irq_map[ch]);
                if (!success) {
                    LOG_VMM_ERR("IRQ %d dropped on vCPU %d\n", passthrough_irq_map[ch], GUEST_VCPU_ID);
                }
                break;
            }
            printf("Unexpected channel, ch: 0x%lx\n", ch);
    }
}

/*
 * The primary purpose of the VMM after initialisation is to act as a fault-handler,
 * whenever our guest causes an exception, it gets delivered to this entry point for
 * the VMM to handle.
 */
void fault(microkit_id id, microkit_msginfo msginfo) {
    bool success = fault_handle(id, msginfo);
    if (success) {
        /* Now that we have handled the fault successfully, we reply to it so
         * that the guest can resume execution. */
        microkit_fault_reply(microkit_msginfo_new(0, 0));
    }
}
