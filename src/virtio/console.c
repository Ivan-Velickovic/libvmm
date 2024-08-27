/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libvmm/virq.h>
#include <libvmm/util/util.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/mmio.h>
#include <libvmm/virtio/console.h>
#include <sddf/serial/queue.h>

/* Uncomment this to enable debug logging */
// #define DEBUG_CONSOLE

#if defined(DEBUG_CONSOLE)
#define LOG_CONSOLE(...) do{ printf("VIRTIO(CONSOLE): "); printf(__VA_ARGS__); }while(0)
#else
#define LOG_CONSOLE(...) do{}while(0)
#endif

#define LOG_CONSOLE_ERR(...) do{ printf("VIRTIO(CONSOLE)|ERROR: "); printf(__VA_ARGS__); }while(0)

static inline struct virtio_console_device *device_state(struct virtio_device *dev)
{
    return (struct virtio_console_device *)dev->device_data;
}

static void virtio_console_features_print(uint32_t features)
{
    /* Dump the features given in a human-readable format */
    LOG_CONSOLE("Dumping features (0x%lx):\n", features);
    LOG_CONSOLE("feature VIRTIO_CONSOLE_F_SIZE set to %s\n",
                BIT_LOW(VIRTIO_CONSOLE_F_SIZE) & features ? "true" : "false");
    LOG_CONSOLE("feature VIRTIO_CONSOLE_F_MULTIPORT set to %s\n",
                BIT_LOW(VIRTIO_CONSOLE_F_MULTIPORT) & features ? "true" : "false");
    LOG_CONSOLE("feature VIRTIO_CONSOLE_F_EMERG_WRITE set to %s\n",
                BIT_LOW(VIRTIO_CONSOLE_F_EMERG_WRITE) & features ? "true" : "false");
}

static void virtio_console_reset(struct virtio_device *dev)
{
    LOG_CONSOLE("operation: reset device\n");

    for (int i = 0; i < dev->num_vqs; i++) {
        dev->vqs[i].ready = false;
        dev->vqs[i].last_idx = 0;
    }
}

static bool virtio_console_get_device_features(struct virtio_device *dev, uint32_t *features)
{
    LOG_CONSOLE("operation: get device features\n");

    switch (dev->data.DeviceFeaturesSel) {
    case 0:
        *features = 0;
        break;
    case 1:
        *features = BIT_HIGH(VIRTIO_F_VERSION_1);
        break;
    default:
        LOG_CONSOLE_ERR("driver sets DeviceFeaturesSel to 0x%x, which doesn't make sense\n", dev->data.DeviceFeaturesSel);
        return false;
    }

    return true;
}

static bool virtio_console_set_driver_features(struct virtio_device *dev, uint32_t features)
{
    LOG_CONSOLE("operation: set driver features\n");
    virtio_console_features_print(features);

    bool success = false;

    switch (dev->data.DriverFeaturesSel) {
    // feature bits 0 to 31
    case 0:
        /* We do not offer any features in the first 32-bit bits */
        success = (features == 0);
        break;
    // features bits 32 to 63
    case 1:
        success = (features == BIT_HIGH(VIRTIO_F_VERSION_1));
        break;
    default:
        LOG_CONSOLE_ERR("driver sets DriverFeaturesSel to 0x%x, which doesn't make sense\n", dev->data.DriverFeaturesSel);
        return false;
    }

    if (success) {
        dev->data.features_happy = 1;
        LOG_CONSOLE("device is feature happy\n");
    }

    return success;
}

static bool virtio_console_get_device_config(struct virtio_device *dev, uint32_t offset, uint32_t *config)
{
    LOG_CONSOLE("operation: get device config\n");
    return false;
}

static bool virtio_console_set_device_config(struct virtio_device *dev, uint32_t offset, uint32_t config)
{
    LOG_CONSOLE("operation: set device config\n");
    return false;
}

static bool virtio_console_handle_tx(struct virtio_device *dev)
{
    LOG_CONSOLE("operation: handle transmit\n");
    // @ivanv: we need to check the pre-conditions before doing anything. e.g check
    // TX_QUEUE is ready?
    assert(dev->num_vqs > TX_QUEUE);
    struct virtio_queue_handler *vq = &dev->vqs[TX_QUEUE];
    struct virtio_console_device *console = device_state(dev);

    /* Transmit all available descriptors possible */
    LOG_CONSOLE("processing available buffers from index [0x%lx..0x%lx)\n", vq->last_idx, vq->virtq.avail->idx);
    bool transferred = false;
    while (vq->last_idx != vq->virtq.avail->idx && !serial_queue_full(&console->txq, console->txq.queue->head)) {
        uint16_t desc_idx = vq->virtq.avail->ring[vq->last_idx % vq->virtq.num];
        struct virtq_desc desc;
        /* Traverse chained descriptors */
        do {
            desc = vq->virtq.desc[desc_idx];
            // @ivanv: to the debug logging, we should actually print out the buffer contents
            LOG_CONSOLE("processing descriptor (0x%lx) with buffer [0x%lx..0x%lx)\n", desc_idx, desc.addr, desc.addr + desc.len);

            uint32_t bytes_remain = desc.len;
            /* Copy all contiguous data */
            while (bytes_remain > 0 && !serial_queue_full(&console->txq, console->txq.queue->head)) {
                uint32_t free = serial_queue_contiguous_free(&console->txq);
                uint32_t to_transfer = (bytes_remain < free) ? bytes_remain : free;
                if (to_transfer) {
                    transferred = true;
                }

                memcpy(console->txq.data_region + (console->txq.queue->tail % console->txq.size),
                       (char *)(desc.addr + (desc.len - bytes_remain)), to_transfer);

                serial_update_visible_tail(&console->txq, console->txq.queue->tail + to_transfer);
                bytes_remain -= to_transfer;
            }

            desc_idx = desc.next;

        } while (desc.flags & VIRTQ_DESC_F_NEXT && !serial_queue_full(&console->txq, console->txq.queue->head));

        struct virtq_used_elem used_elem = {vq->virtq.avail->ring[vq->last_idx % vq->virtq.num], 0};
        vq->virtq.used->ring[vq->virtq.used->idx % vq->virtq.num] = used_elem;
        vq->virtq.used->idx++;

        vq->last_idx++;
    }

    /* While unlikely, it is possible that we could not consume any of the
     * available data. In this case we do not set the IRQ status. */
    if (transferred) {
        dev->data.InterruptStatus = BIT_LOW(0);
        bool success = virq_inject(dev->virq);
        assert(success);

        if (serial_require_producer_signal(&console->txq)) {
            serial_cancel_producer_signal(&console->txq);
            microkit_notify(console->tx_ch);
        }

        return success;
    }

    return true;
}

bool virtio_console_handle_rx(struct virtio_console_device *console)
{
    LOG_CONSOLE("operation: handle rx\n");
    assert(console->virtio_device.num_vqs > RX_QUEUE);

    /* Used to know whether to set the IRQ status. */
    bool transferred = false;

    bool reprocess = true;
    while (reprocess) {
        struct virtio_queue_handler *vq = &console->virtio_device.vqs[RX_QUEUE];
        LOG_CONSOLE("processing available buffers from index [0x%lx..0x%lx)\n", vq->last_idx, vq->virtq.avail->idx);
        while (vq->last_idx != vq->virtq.avail->idx && !serial_queue_empty(&console->rxq, console->rxq.queue->head)) {
            transferred = true;

            uint16_t desc_head = vq->virtq.avail->ring[vq->last_idx % vq->virtq.num];
            struct virtq_desc desc = vq->virtq.desc[desc_head];
            LOG_CONSOLE("processing descriptor (0x%lx) with buffer [0x%lx..0x%lx)\n", desc_head, desc.addr, desc.addr + desc.len);
            uint32_t bytes_written = 0;
            char c;
            while (bytes_written < desc.len && !serial_dequeue(&console->rxq, &console->rxq.queue->head, &c)) {
                *(char *)(desc.addr + bytes_written) = c;
                bytes_written++;
            }

            struct virtq_used_elem used_elem = {desc_head, bytes_written};
            vq->virtq.used->ring[vq->virtq.used->idx % vq->virtq.num] = used_elem;
            vq->virtq.used->idx++;

            vq->last_idx++;
        }

        serial_request_producer_signal(&console->rxq);
        reprocess = false;

        if (vq->last_idx != vq->virtq.avail->idx && !serial_queue_empty(&console->rxq, console->rxq.queue->head)) {
            serial_cancel_producer_signal(&console->rxq);
            reprocess = true;
        }
    }

    /* While unlikely, it is possible that we could not consume any of the
     * available data. In this case we do not set the IRQ status. */
    if (transferred) {
        console->virtio_device.data.InterruptStatus = BIT_LOW(0);
        bool success = virq_inject(console->virtio_device.virq);
        assert(success);

        return success;
    }

    return true;
}

virtio_device_funs_t functions = {
    .device_reset = virtio_console_reset,
    .get_device_features = virtio_console_get_device_features,
    .set_driver_features = virtio_console_set_driver_features,
    .get_device_config = virtio_console_get_device_config,
    .set_device_config = virtio_console_set_device_config,
    .queue_notify = virtio_console_handle_tx,
};

bool virtio_mmio_console_init(struct virtio_console_device *console,
                              uintptr_t region_base,
                              uintptr_t region_size,
                              size_t virq,
                              serial_queue_handle_t *rxq,
                              serial_queue_handle_t *txq,
                              int tx_ch)
{
    struct virtio_device *dev = &console->virtio_device;
    dev->data.DeviceID = DEVICE_ID_VIRTIO_CONSOLE;
    dev->data.VendorID = VIRTIO_MMIO_DEV_VENDOR_ID;
    dev->funs = &functions;
    dev->vqs = console->vqs;
    dev->num_vqs = VIRTIO_CONSOLE_NUM_VIRTQ;
    dev->virq = virq;
    dev->device_data = console;

    console->rxq = *rxq;
    console->txq = *txq;
    console->tx_ch = tx_ch;

    return virtio_mmio_register_device(dev, region_base, region_size, virq);
}
