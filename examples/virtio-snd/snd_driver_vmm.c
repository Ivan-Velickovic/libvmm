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
#include <libvmm/util/util.h>
#include <libvmm/virtio/virtio.h>
#include <libvmm/arch/aarch64/linux.h>
#include <libvmm/arch/aarch64/fault.h>
#include <sddf/serial/queue.h>
#include <serial_config.h>
#include <sddf/sound/queue.h>
#include <sddf/util/cache.h>
#include <uio/sound.h>

/*
 * As this is just an example, for simplicity we just make the size of the
 * guest's "RAM" the same for all platforms. For just booting Linux with a
 * simple user-space, 0x10000000 bytes (256MB) is plenty.
 */
#define GUEST_RAM_SIZE 0x10000000

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

#define SND_CLIENT_CH 4

#define UIO_SND_IRQ 50

#define MAX_IRQ_CH 63
int passthrough_irq_map[MAX_IRQ_CH];

#define SERIAL_TX_CH 1
#define SERIAL_RX_CH 2

#define VIRTIO_CONSOLE_IRQ (74)
#define VIRTIO_CONSOLE_BASE (0x130000)
#define VIRTIO_CONSOLE_SIZE (0x1000)

serial_queue_t *serial_rx_queue;
serial_queue_t *serial_tx_queue;

char *serial_rx_data;
char *serial_tx_data;

uintptr_t sound_shared_state;
uintptr_t sound_data_paddr;

static struct virtio_console_device virtio_console;

static void passthrough_device_ack(size_t vcpu_id, int irq, void *cookie)
{
    microkit_channel irq_ch = (microkit_channel)(int64_t)cookie;
    microkit_irq_ack(irq_ch);
}

static void register_passthrough_irq(int irq, microkit_channel irq_ch)
{
    LOG_VMM("Register passthrough IRQ %d (channel: 0x%lx)\n", irq, irq_ch);
    assert(irq_ch < MAX_IRQ_CH);
    passthrough_irq_map[irq_ch] = irq;

    int err = virq_register(GUEST_BOOT_VCPU_ID, irq, &passthrough_device_ack, (void *)(int64_t)irq_ch);
    if (!err) {
        LOG_VMM_ERR("Failed to register IRQ %d\n", irq);
        return;
    }
}

static bool uio_sound_fault_handler(size_t vcpu_id,
                                    size_t offset,
                                    size_t fsr,
                                    seL4_UserContext *regs,
                                    void *data)
{
    microkit_notify(SND_CLIENT_CH);
    return true;
}

static void uio_sound_virq_ack(size_t vcpu_id, int irq, void *cookie) {}

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
    bool success = virq_controller_init(GUEST_BOOT_VCPU_ID);
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
    serial_cli_queue_init_sys(microkit_name, &serial_rxq, serial_rx_queue, serial_rx_data, &serial_txq, serial_tx_queue,
                              serial_tx_data);

    /* Initialise virtIO console device */
    success = virtio_mmio_console_init(&virtio_console,
                                       VIRTIO_CONSOLE_BASE,
                                       VIRTIO_CONSOLE_SIZE,
                                       VIRTIO_CONSOLE_IRQ,
                                       &serial_rxq, &serial_txq,
                                       SERIAL_TX_CH);
    assert(success);

    success = virq_register(GUEST_BOOT_VCPU_ID, UIO_SND_IRQ, &uio_sound_virq_ack, NULL);
    assert(success);

    success = fault_register_vm_exception_handler(UIO_SND_FAULT_ADDRESS,
                                                  sizeof(size_t),
                                                  &uio_sound_fault_handler, NULL);
    assert(success);

#if defined(BOARD_qemu_virt_aarch64)
    register_passthrough_irq(37, 5); // Serial
#elif defined(BOARD_odroidc4)
    register_passthrough_irq(48, 5); // USB controller
    register_passthrough_irq(63, 6); // USB 1
    register_passthrough_irq(62, 7); // USB 2
    register_passthrough_irq(5, 8);  // Unknown
#else
#error Need to define passthrough IRQs
#endif

    uintptr_t *data_paddr = &((vm_shared_state_t *)sound_shared_state)->data_paddr;
    *data_paddr = sound_data_paddr;
    cache_clean((uintptr_t)data_paddr, sizeof(uintptr_t));

    /* Finally start the guest */
    guest_start(GUEST_BOOT_VCPU_ID, kernel_pc, GUEST_DTB_VADDR, GUEST_INIT_RAM_DISK_VADDR);
}

void notified(microkit_channel ch)
{
    bool success;

    switch (ch) {
    case SERIAL_RX_CH:
        /* We have received an event from the serial virtualiser, so we
            * call the virtIO console handling */
        virtio_console_handle_rx(&virtio_console);
        break;
    case SND_CLIENT_CH:
        success = virq_inject(UIO_SND_IRQ);
        if (!success) {
            LOG_VMM_ERR("IRQ %d dropped\n", UIO_SND_IRQ);
        }
        break;
    default:
        if (passthrough_irq_map[ch]) {
            success = virq_inject(passthrough_irq_map[ch]);
            if (!success) {
                LOG_VMM_ERR("IRQ %d dropped\n", passthrough_irq_map[ch]);
            }
        } else {
            printf("Unexpected channel, ch: 0x%lx\n", ch);
        }
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
