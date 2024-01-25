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
#include "virtio/virtio.h"
#include "virtio/console.h"
#include <sddf/serial/shared_ringbuffer.h>
#include "virtio/block.h"
#include <sddf/blk/shared_queue.h>

/*
 * As this is just an example, for simplicity we just make the size of the
 * guest's "RAM" the same for all platforms. For just booting Linux with a
 * simple user-space, 0x10000000 bytes (256MB) is plenty.
 */
#define GUEST_RAM_SIZE 0x10000000

#if defined(BOARD_qemu_arm_virt)
#define GUEST_DTB_VADDR 0x47000000
#define GUEST_INIT_RAM_DISK_VADDR 0x46000000
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

/* Virtio Console */
#define SERIAL_MUX_TX_CH 1
#define SERIAL_MUX_RX_CH 2

#define VIRTIO_CONSOLE_IRQ (74)
#define VIRTIO_CONSOLE_BASE (0x130000)
#define VIRTIO_CONSOLE_SIZE (0x1000)

uintptr_t serial_rx_free;
uintptr_t serial_rx_used;
uintptr_t serial_tx_free;
uintptr_t serial_tx_used;

uintptr_t serial_rx_data;
uintptr_t serial_tx_data;

size_t serial_ch[SDDF_SERIAL_NUM_CH];

ring_handle_t serial_rx_ring_handle;
ring_handle_t serial_tx_ring_handle;

static ring_handle_t *serial_ring_handles[SDDF_SERIAL_NUM_HANDLES];

static struct virtio_device virtio_console;

/* Virtio Block */
#define BLK_CH 3

#define VIRTIO_BLK_IRQ (75)
#define VIRTIO_BLK_BASE (0x150000)
#define VIRTIO_BLK_SIZE (0x1000)

uintptr_t blk_req_queue;
uintptr_t blk_resp_queue;
uintptr_t blk_data;
blk_queue_handle_t blk_queue_handle;
static blk_queue_handle_t *blk_queue_handles[SDDF_BLK_NUM_HANDLES];

size_t blk_ch[SDDF_BLK_NUM_CH];
blk_storage_info_t blk_config;

blk_data_region_t blk_data_region;
bitarray_t blk_data_region_avail_bitarr;
word_t blk_data_region_avail_bitarr_words[roundup_bits2words64(SDDF_BLK_MAX_DATA_BUFFERS)];
static blk_data_region_t *blk_data_region_handlers[SDDF_BLK_NUM_HANDLES];

static struct virtio_device virtio_blk;

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
    
    /* Initialise our sDDF ring buffers for the serial device */
    ring_init(&serial_rx_ring_handle,
            (ring_buffer_t *)serial_rx_free,
            (ring_buffer_t *)serial_rx_used,
            true,
            NUM_BUFFERS,
            NUM_BUFFERS);
    for (int i = 0; i < NUM_BUFFERS - 1; i++) {
        int ret = enqueue_free(&serial_rx_ring_handle, serial_rx_data + (i * BUFFER_SIZE), BUFFER_SIZE, NULL);
        if (ret != 0) {
            microkit_dbg_puts(microkit_name);
            microkit_dbg_puts(": server rx buffer population, unable to enqueue buffer\n");
        }
    }
    ring_init(&serial_tx_ring_handle,
            (ring_buffer_t *)serial_tx_free,
            (ring_buffer_t *)serial_tx_used,
            true,
            NUM_BUFFERS,
            NUM_BUFFERS);
    for (int i = 0; i < NUM_BUFFERS - 1; i++) {
        // Have to start at the memory region left of by the rx ring
        int ret = enqueue_free(&serial_tx_ring_handle, serial_tx_data + ((i + NUM_BUFFERS) * BUFFER_SIZE), BUFFER_SIZE, NULL);
        assert(ret == 0);
        if (ret != 0) {
            microkit_dbg_puts(microkit_name);
            microkit_dbg_puts(": server tx buffer population, unable to enqueue buffer\n");
        }
    }
    serial_ring_handles[SDDF_SERIAL_RX_RING] = &serial_rx_ring_handle;
    serial_ring_handles[SDDF_SERIAL_TX_RING] = &serial_tx_ring_handle;
    /* Neither ring should be plugged and hence all buffers we send should actually end up at the driver. */
    assert(!ring_plugged(serial_tx_ring_handle.free_ring));
    assert(!ring_plugged(serial_tx_ring_handle.used_ring));
    /* Initialise channel */
    serial_ch[SDDF_SERIAL_TX_CH_INDEX] = SERIAL_MUX_TX_CH;
    /* Initialise virtIO console device */
    success = virtio_mmio_device_init(&virtio_console, CONSOLE, VIRTIO_CONSOLE_BASE, VIRTIO_CONSOLE_SIZE, VIRTIO_CONSOLE_IRQ,
                                      NULL, NULL, (void **)serial_ring_handles, serial_ch);
    assert(success);
    
    /* Initialise our sDDF ring buffers for the block device */
    blk_queue_init(&blk_queue_handle,
                (blk_req_queue_t *)blk_req_queue,
                (blk_resp_queue_t *)blk_resp_queue,
                true,
                BLK_REQ_QUEUE_SIZE,
                BLK_RESP_QUEUE_SIZE);
    blk_queue_handles[SDDF_BLK_DEFAULT_HANDLE] = &blk_queue_handle;
    /* Request queue should be plugged and hence all buffers we send should actually end up at the driver VM. */
    assert(!blk_req_queue_plugged(&blk_queue_handle));
    /* Initialise bit array */
    bitarray_init(&blk_data_region_avail_bitarr, blk_data_region_avail_bitarr_words, roundup_bits2words64(SDDF_BLK_MAX_DATA_BUFFERS));
    /* Data struct that handles allocation and freeing of data buffers in sDDF shared memory region */
    blk_data_region.avail_bitpos = 0; /* bit position of next avail buffer */
    blk_data_region.avail_bitarr = &blk_data_region_avail_bitarr; /* bit array representing avail data buffers */
    blk_data_region.num_buffers = SDDF_BLK_MAX_DATA_BUFFERS; /* number of buffers in data region */
    blk_data_region.addr = blk_data; /* encoded base address of data region */
    /* Set all available bits to 1 to indicate it is available */ 
    bitarray_set_region(&blk_data_region_avail_bitarr, 0, SDDF_BLK_MAX_DATA_BUFFERS);
    blk_data_region_handlers[SDDF_BLK_DEFAULT_HANDLE] = &blk_data_region;
    /* Initialise channel */
    blk_ch[SDDF_BLK_DEFAULT_CH_INDEX] = BLK_CH; 
    // @ericc: Grab these values from driver in the future, for now hard code here
    blk_config.blocksize = 1024;
    blk_config.size = 10000;
    blk_config.ready = true;
    /* Initialise virtIO block device */
    success = virtio_mmio_device_init(&virtio_blk, BLK, VIRTIO_BLK_BASE, VIRTIO_BLK_SIZE, VIRTIO_BLK_IRQ,
                                      &blk_config, (void **)blk_data_region_handlers, (void **)blk_queue_handles, blk_ch);
    assert(success);

    /* Finally start the guest */
    guest_start(GUEST_VCPU_ID, kernel_pc, GUEST_DTB_VADDR, GUEST_INIT_RAM_DISK_VADDR);
}

void notified(microkit_channel ch) {
    switch (ch) {
        case SERIAL_MUX_RX_CH: {
            /* We have received an event from the serial multipelxor, so we
             * call the virtIO console handling */
            virtio_console_handle_rx(&virtio_console);
            break;
        }
        case BLK_CH: {
            virtio_blk_handle_resp(&virtio_blk);
            break;
        }
        default:
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
