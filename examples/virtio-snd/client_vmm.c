/*
 * Copyright 2023, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stddef.h>
#include <stdint.h>
#include <microkit.h>
#include <libvmm/guest.h>
#include <libvmm/virq.h>
#include <libvmm/util/atomic.h>
#include <libvmm/util/util.h>
#include <libvmm/virtio/virtio.h>
#include <libvmm/arch/aarch64/linux.h>
#include <libvmm/arch/aarch64/fault.h>
#include <sddf/serial/queue.h>
#include <serial_config.h>
#include <sddf/sound/queue.h>

#if defined(BOARD_qemu_virt_aarch64)
#define GUEST_DTB_VADDR 0x47000000
#define GUEST_INIT_RAM_DISK_VADDR 0x46000000
#elif defined(BOARD_odroidc4)
#define GUEST_DTB_VADDR 0x27000000
#define GUEST_INIT_RAM_DISK_VADDR 0x26000000
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
#define SERIAL_VIRT_TX_CH 1
#define SERIAL_VIRT_RX_CH 2

#define VIRTIO_CONSOLE_IRQ (74)
#define VIRTIO_CONSOLE_BASE (0x130000)
#define VIRTIO_CONSOLE_SIZE (0x1000)

#define VIRTIO_SOUND_IRQ (76)
#define VIRTIO_SOUND_BASE (0x170000)
#define VIRTIO_SOUND_SIZE (0x1000)

#define SOUND_DRIVER_CH 4

serial_queue_t *serial_rx_queue;
serial_queue_t *serial_tx_queue;

char *serial_rx_data;
char *serial_tx_data;

uintptr_t sound_cmd_req;
uintptr_t sound_cmd_res;
uintptr_t sound_pcm_req;
uintptr_t sound_pcm_res;

uintptr_t sound_data;
uintptr_t sound_shared_state;

static struct virtio_console_device virtio_console;
static struct virtio_snd_device virtio_sound;

uintptr_t kernel_pc = 0;

void init(void)
{
    /* Initialise the VMM, the VCPU(s), and start the guest */
    LOG_VMM("starting \"%s\"\n", microkit_name);
    /* Place all the binaries in the right locations before starting the guest */
    size_t kernel_size = _guest_kernel_image_end - _guest_kernel_image;
    size_t dtb_size = _guest_dtb_image_end - _guest_dtb_image;
    size_t initrd_size = _guest_initrd_image_end - _guest_initrd_image;
    kernel_pc = linux_setup_images(guest_ram_vaddr,
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
    bool success = virq_controller_init();
    if (!success) {
        LOG_VMM_ERR("Failed to initialise emulated interrupt controller\n");
        return;
    }

    /* Initialise our sDDF ring buffers for the serial device */
    serial_queue_handle_t serial_rxq, serial_txq;
    serial_cli_queue_init_sys(microkit_name, &serial_rxq, serial_rx_queue, serial_rx_data, &serial_txq, serial_tx_queue,
                              serial_tx_data);

    /* Initialise virtIO console device */
    success = virtio_mmio_console_init(&virtio_console,
                                       VIRTIO_CONSOLE_BASE,
                                       VIRTIO_CONSOLE_SIZE,
                                       VIRTIO_CONSOLE_IRQ,
                                       &serial_rxq, &serial_txq,
                                       SERIAL_VIRT_TX_CH);
    assert(success);

    assert(sound_cmd_req);
    assert(sound_cmd_res);
    assert(sound_pcm_req);
    assert(sound_pcm_res);
    assert(sound_data);

    sound_queues_t sound_queues;

    sound_queues_init(&sound_queues,
                      (void *)sound_cmd_req,
                      (void *)sound_cmd_res,
                      (void *)sound_pcm_req,
                      (void *)sound_pcm_res,
                      SOUND_CMD_QUEUE_SIZE,
                      SOUND_PCM_QUEUE_SIZE);

    sound_queues_init_buffers(&sound_queues);

    /* Don't start the guest until driver VM is ready. */
    sound_shared_state_t *shared_state = (void *)sound_shared_state;

    while (!ATOMIC_LOAD(&shared_state->ready, __ATOMIC_ACQUIRE));

    success = virtio_mmio_snd_init(&virtio_sound,
                                   VIRTIO_SOUND_BASE,
                                   VIRTIO_SOUND_SIZE,
                                   VIRTIO_SOUND_IRQ,
                                   shared_state,
                                   &sound_queues,
                                   sound_data,
                                   SOUND_DRIVER_CH);
    assert(success);

    success = guest_start(GUEST_BOOT_VCPU_ID, kernel_pc, GUEST_DTB_VADDR, GUEST_INIT_RAM_DISK_VADDR);
    assert(success);
}

void notified(microkit_channel ch)
{
    switch (ch) {
    case SERIAL_VIRT_RX_CH: {
        /* We have received an event from the serial virtualiser, so we
         * call the virtIO console handling */
        virtio_console_handle_rx(&virtio_console);
        break;
    }
    case SOUND_DRIVER_CH: {
        virtio_snd_notified(&virtio_sound);
        break;
    }
    default:
        printf("Unexpected channel, ch: 0x%lx\n", ch);
    }
}

seL4_Bool fault(microkit_child child, microkit_msginfo msginfo, microkit_msginfo *reply_msginfo)
{
    bool success = fault_handle(child, msginfo);
    if (success) {
        /* Now that we have handled the fault successfully, we reply to it so
         * that the guest can resume execution. */
        *reply_msginfo = microkit_msginfo_new(0, 0);
        return seL4_True;
    }

    return seL4_False;
}
