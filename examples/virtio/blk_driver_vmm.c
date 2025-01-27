/*
 * Copyright 2023, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <blk_config.h>
#include <libvmm/arch/aarch64/fault.h>
#include <libvmm/arch/aarch64/linux.h>
#include <libvmm/guest.h>
#include <libvmm/uio/uio.h>
#include <libvmm/util/util.h>
#include <libvmm/virq.h>
#include <libvmm/virtio/virtio.h>
#include <microkit.h>
#include <sddf/serial/queue.h>
#include <serial_config.h>
#include <stddef.h>
#include <stdint.h>
#include <uio/blk.h>

#define GUEST_RAM_SIZE 0x6000000

#if defined(BOARD_qemu_virt_aarch64)
#define GUEST_DTB_VADDR 0x47f00000
#define GUEST_INIT_RAM_DISK_VADDR 0x47000000
#elif defined(BOARD_odroidc4)
#define GUEST_DTB_VADDR 0x25f10000
#define GUEST_INIT_RAM_DISK_VADDR 0x24000000
#else
#error Need to define guest kernel image address and DTB address
#endif

/* Data for the guest's kernel image. */
extern char _guest_kernel_image[];
extern char _guest_kernel_image_end[];
/* Data for the device tree to be passed to the kernel. */
extern char _guest_dtb_image[];
extern char _guest_dtb_image_end[];
/* Data for the initial RAM disk to be passed to the kernel. */
extern char _guest_initrd_image[];
extern char _guest_initrd_image_end[];
/* Microkit will set this variable to the start of the guest RAM memory region. */
uintptr_t guest_ram_vaddr;

/* Passing info from VMM to block uio driver */
driver_blk_vmm_info_passing_t *driver_blk_vmm_info_passing;
uintptr_t virt_blk_data;
uintptr_t client_vmm_1_blk_data;
uintptr_t client_vmm_2_blk_data;

/* sDDF block */
#define BLOCK_CH 1
#if defined(BOARD_odroidc4)
#define SD_IRQ 222
#elif defined(BOARD_qemu_virt_aarch64)
#define BLOCK_IRQ 79
#endif

#define UIO_IRQ 50
#define UIO_CH 3

/* This global is kind of redundant, but for now it's needed to be passed
 * through to the uio-vmm notify handler
 */
microkit_channel uio_ch = UIO_CH;

/* Serial */
#define SERIAL_VIRT_TX_CH 4
#define SERIAL_VIRT_RX_CH 5

#define VIRTIO_CONSOLE_IRQ (74)
#define VIRTIO_CONSOLE_BASE (0x130000)
#define VIRTIO_CONSOLE_SIZE (0x1000)

serial_queue_t *serial_rx_queue;
serial_queue_t *serial_tx_queue;

char *serial_rx_data;
char *serial_tx_data;

static struct virtio_console_device virtio_console;

void init(void)
{
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

    assert(serial_rx_data);
    assert(serial_tx_data);
    assert(serial_rx_queue);
    assert(serial_tx_queue);

    /* Initialise our sDDF ring buffers for the serial device */
    serial_queue_handle_t serial_rxq, serial_txq;
    serial_cli_queue_init_sys(microkit_name, &serial_rxq, serial_rx_queue, serial_rx_data, &serial_txq, serial_tx_queue, serial_tx_data);


    /* Initialise virtIO console device */
    success = virtio_mmio_console_init(&virtio_console,
                                  VIRTIO_CONSOLE_BASE,
                                  VIRTIO_CONSOLE_SIZE,
                                  VIRTIO_CONSOLE_IRQ,
                                  &serial_rxq, &serial_txq,
                                  SERIAL_VIRT_TX_CH);
    assert(success);

    /* Register the block uio driver */
    success = uio_register_driver(UIO_IRQ, &uio_ch, 0x80000000, 0x1000);
    assert(success);

    /* Populate vmm info passing to block uio driver */
    driver_blk_vmm_info_passing->client_data_phys[0] = virt_blk_data;
    driver_blk_vmm_info_passing->client_data_phys[1] = client_vmm_1_blk_data;
    driver_blk_vmm_info_passing->client_data_phys[2] = client_vmm_2_blk_data;
    driver_blk_vmm_info_passing->client_data_size[0] =
        BLK_DATA_REGION_SIZE_DRIV;
    driver_blk_vmm_info_passing->client_data_size[1] =
        BLK_DATA_REGION_SIZE_CLI0;
    driver_blk_vmm_info_passing->client_data_size[2] =
        BLK_DATA_REGION_SIZE_CLI1;

#if defined(BOARD_odroidc4)
    /* Register the SD card IRQ */
    virq_register_passthrough(GUEST_VCPU_ID, SD_IRQ, BLOCK_CH);
#endif

#if defined(BOARD_qemu_virt_aarch64)
    /* Register the block device IRQ */
    virq_register_passthrough(GUEST_VCPU_ID, BLOCK_IRQ, BLOCK_CH);
#endif

    /* Finally start the guest */
    guest_start(GUEST_VCPU_ID, kernel_pc, GUEST_DTB_VADDR, GUEST_INIT_RAM_DISK_VADDR);
}

void notified(microkit_channel ch)
{
    bool handled = false;

    handled = virq_handle_passthrough(ch);

    switch (ch) {
    case UIO_CH: {
        int success = virq_inject(GUEST_VCPU_ID, UIO_IRQ);
        if (!success) {
            LOG_VMM_ERR("Failed to inject UIO IRQ 0x%lx\n", UIO_IRQ);
        }
        handled = true;
        break;
    }
    case SERIAL_VIRT_RX_CH:
        virtio_console_handle_rx(&virtio_console);
        break;
    }

    if (!handled) {
        LOG_VMM_ERR("Unhandled notification on channel %d\n", ch);
    }
}

seL4_Bool fault(microkit_child child, microkit_msginfo msginfo, microkit_msginfo *reply_msginfo) {
    bool success = fault_handle(child, msginfo);
    if (success) {
        /* Now that we have handled the fault successfully, we reply to it so
         * that the guest can resume execution. */
        *reply_msginfo = microkit_msginfo_new(0, 0);
        return seL4_True;
    }

    return seL4_False;
}
