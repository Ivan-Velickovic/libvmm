/*
 * Copyright 2023, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <microkit.h>
#include <libvmm/virq.h>
#include <libvmm/util/util.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/mmio.h>
#include <libvmm/virtio/virtq.h>
#include <libvmm/arch/aarch64/fault.h>

/* Uncomment this to enable debug logging */
// #define DEBUG_MMIO

// @ivanv: use this logging instead of having commented printfs
#if defined(DEBUG_MMIO)
#define LOG_MMIO(...) do{ printf("%s|VIRTIO(MMIO): ", microkit_name); printf(__VA_ARGS__); }while(0)
#else
#define LOG_MMIO(...) do{}while(0)
#endif

// @jade: add some giant comments about this file
// generic virtio mmio emul interface

#define REG_RANGE(r0, r1)   r0 ... (r1 - 1)

struct virtq *get_current_virtq_by_handler(virtio_device_t *dev)
{
    assert(dev->data.QueueSel < dev->num_vqs);
    return &dev->vqs[dev->data.QueueSel].virtq;
}

/*
 * Protocol for device status changing can be found in section
 * '3.1 Device Initialization' of the virtIO specification.
 */
// @ivanv: why is this function necessary, why not just use data.Status?
int handle_virtio_mmio_get_status_flag(virtio_device_t *dev, uint32_t *retreg)
{
    *retreg = dev->data.Status;
    return 1;
}

int handle_virtio_mmio_set_status_flag(virtio_device_t *dev, uint32_t reg)
{
    int success = 1;

    // we only care about the new status
    dev->data.Status &= reg;
    reg ^= dev->data.Status;
    // printf("VIRTIO MMIO|INFO: set status flag 0x%x.\n", reg);

    switch (reg) {
    case VIRTIO_CONFIG_S_RESET:
        dev->data.Status = 0;
        dev->funs->device_reset(dev);
        break;

    case VIRTIO_CONFIG_S_ACKNOWLEDGE:
        // are we following the initialization protocol?
        if (dev->data.Status == 0) {
            dev->data.Status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
            // nothing to do from our side (as the virtio device).
        }
        break;

    case VIRTIO_CONFIG_S_DRIVER:
        // are we following the initialization protocol?
        if (dev->data.Status & VIRTIO_CONFIG_S_ACKNOWLEDGE) {
            dev->data.Status |= VIRTIO_CONFIG_S_DRIVER;
            // nothing to do from our side (as the virtio device).
        }
        break;

    case VIRTIO_CONFIG_S_FEATURES_OK:
        // are we following the initialization protocol?
        if (dev->data.Status & VIRTIO_CONFIG_S_DRIVER) {
            // are features OK?
            dev->data.Status |= (dev->data.features_happy ? VIRTIO_CONFIG_S_FEATURES_OK : 0);
        }
        break;

    case VIRTIO_CONFIG_S_DRIVER_OK:
        dev->data.Status |= VIRTIO_CONFIG_S_DRIVER_OK;
        // probably do some san checks here
        break;

    case VIRTIO_CONFIG_S_FAILED:
        printf("VIRTIO MMIO|INFO: received FAILED status from driver, giving up this device.\n");
        break;

    default:
        printf("VIRTIO MMIO|INFO: unknown device status 0x%x.\n", reg);
        success = 0;
    }
    return success;
}

static bool handle_virtio_mmio_reg_read(virtio_device_t *dev, size_t vcpu_id, size_t offset, size_t fsr,
                                        seL4_UserContext *regs)
{

    uint32_t reg = 0;
    bool success = true;
    LOG_MMIO("read from 0x%lx\n", offset);

    switch (offset) {
    case REG_RANGE(REG_VIRTIO_MMIO_MAGIC_VALUE, REG_VIRTIO_MMIO_VERSION):
        reg = VIRTIO_MMIO_DEV_MAGIC;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_VERSION, REG_VIRTIO_MMIO_DEVICE_ID):
        reg = VIRTIO_MMIO_DEV_VERSION;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_DEVICE_ID, REG_VIRTIO_MMIO_VENDOR_ID):
        reg = dev->data.DeviceID;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_VENDOR_ID, REG_VIRTIO_MMIO_DEVICE_FEATURES):
        reg = dev->data.VendorID;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_DEVICE_FEATURES, REG_VIRTIO_MMIO_DEVICE_FEATURES_SEL):
        success = dev->funs->get_device_features(dev, &reg);
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_NUM_MAX, REG_VIRTIO_MMIO_QUEUE_NUM):
        reg = QUEUE_SIZE;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_READY, REG_VIRTIO_MMIO_QUEUE_NOTIFY):
        if (dev->data.QueueSel < dev->num_vqs) {
            reg = dev->vqs[dev->data.QueueSel].ready;
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_READY\n", dev->data.QueueSel, dev->num_vqs);
            success = false;
        }
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_INTERRUPT_STATUS, REG_VIRTIO_MMIO_INTERRUPT_ACK):
        reg = dev->data.InterruptStatus;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_STATUS, REG_VIRTIO_MMIO_QUEUE_DESC_LOW):
        success = handle_virtio_mmio_get_status_flag(dev, &reg);
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_CONFIG_GENERATION, REG_VIRTIO_MMIO_CONFIG):
        /* ConfigGeneration will need to be update every time when the device changes any of
         * the device config. Currently we only have virtio net device that doesn't do any update
         * on the device config, so the ConfigGeneration is always 0. I left this comment
         * here as a reminder. */
        // reg = device->data.ConfigGeneration;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_CONFIG, REG_VIRTIO_MMIO_CONFIG + 0x100):
        success = dev->funs->get_device_config(dev, offset, &reg);
        // uint32_t mask = fault_get_data_mask(fault_addr, fsr);
        // printf("\"%s\"|VIRTIO MMIO|INFO: device config offset 0x%x, value 0x%x, mask 0x%x\n", sel4cp_name, offset, reg & mask, mask);
        break;
    default:
        printf("VIRTIO MMIO|INFO: unknown register 0x%x.\n", offset);
        success = false;
    }

    uint32_t mask = fault_get_data_mask(offset, fsr);
    // @ivanv: make it clearer that just passing the offset is okay,
    // possibly just fix the API
    fault_emulate_write(regs, offset, fsr, reg & mask);

    return success;
}

static bool handle_virtio_mmio_reg_write(virtio_device_t *dev, size_t vcpu_id, size_t offset, size_t fsr,
                                         seL4_UserContext *regs)
{
    bool success = true;
    uint32_t data = fault_get_data(regs, fsr);
    uint32_t mask = fault_get_data_mask(offset, fsr);
    /* Mask the data to write */
    data &= mask;

    // printf("\"%s\"|VIRTIO MMIO|INFO: Write to 0x%x.\n", sel4cp_name, offset);

    switch (offset) {
    case REG_RANGE(REG_VIRTIO_MMIO_DEVICE_FEATURES_SEL, REG_VIRTIO_MMIO_DRIVER_FEATURES):
        dev->data.DeviceFeaturesSel = data;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_DRIVER_FEATURES, REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL):
        success = dev->funs->set_driver_features(dev, data);
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_DRIVER_FEATURES_SEL, REG_VIRTIO_MMIO_QUEUE_SEL):
        dev->data.DriverFeaturesSel = data;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_SEL, REG_VIRTIO_MMIO_QUEUE_NUM_MAX):
        dev->data.QueueSel = data;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_NUM, REG_VIRTIO_MMIO_QUEUE_READY): {
        if (dev->data.QueueSel < dev->num_vqs) {
            struct virtq *virtq = get_current_virtq_by_handler(dev);
            virtq->num = (unsigned int)data;
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_NUM\n", dev->data.QueueSel, dev->num_vqs);
            success = false;
        }
        break;
    }
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_READY, REG_VIRTIO_MMIO_QUEUE_NOTIFY):
        if (data == 0x1) {
            dev->vqs[dev->data.QueueSel].ready = true;
            // the virtq is already in ram so we don't need to do any initiation
        }
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_NOTIFY, REG_VIRTIO_MMIO_INTERRUPT_STATUS):
        dev->data.QueueNotify = (uint32_t)data;
        success = dev->funs->queue_notify(dev);
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_INTERRUPT_ACK, REG_VIRTIO_MMIO_STATUS):
        dev->data.InterruptStatus &= ~data;
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_STATUS, REG_VIRTIO_MMIO_QUEUE_DESC_LOW):
        success = handle_virtio_mmio_set_status_flag(dev, data);
        break;
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_DESC_LOW, REG_VIRTIO_MMIO_QUEUE_DESC_HIGH): {
        if (dev->data.QueueSel < dev->num_vqs) {
            struct virtq *virtq = get_current_virtq_by_handler(dev);
            uintptr_t ptr = (uintptr_t)virtq->desc;
            ptr |= data;
            virtq->desc = (struct virtq_desc *)ptr;
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_DESC_LOW\n", dev->data.QueueSel, dev->num_vqs);
            success = false;
        }
        break;
    }
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_DESC_HIGH, REG_VIRTIO_MMIO_QUEUE_AVAIL_LOW): {
        if (dev->data.QueueSel < dev->num_vqs) {
            struct virtq *virtq = get_current_virtq_by_handler(dev);
            uintptr_t ptr = (uintptr_t)virtq->desc;
            ptr |= (uintptr_t)data << 32;
            virtq->desc = (struct virtq_desc *)ptr;
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_DESC_HIGH\n", dev->data.QueueSel, dev->num_vqs);
            success = false;
        }
    }
    break;
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_AVAIL_LOW, REG_VIRTIO_MMIO_QUEUE_AVAIL_HIGH): {
        if (dev->data.QueueSel < dev->num_vqs) {
            struct virtq *virtq = get_current_virtq_by_handler(dev);
            uintptr_t ptr = (uintptr_t)virtq->avail;
            ptr |= data;
            virtq->avail = (struct virtq_avail *)ptr;
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_AVAIL_LOW\n", dev->data.QueueSel, dev->num_vqs);
            success = false;
        }
        break;
    }
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_AVAIL_HIGH, REG_VIRTIO_MMIO_QUEUE_USED_LOW): {
        if (dev->data.QueueSel < dev->num_vqs) {
            struct virtq *virtq = get_current_virtq_by_handler(dev);
            uintptr_t ptr = (uintptr_t)virtq->avail;
            ptr |= (uintptr_t)data << 32;
            virtq->avail = (struct virtq_avail *)ptr;
            // printf("VIRTIO MMIO|INFO: virtq avail 0x%lx\n.", ptr);
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_AVAIL_HIGH\n", dev->data.QueueSel, dev->num_vqs);
            success = false;
        }
        break;
    }
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_USED_LOW, REG_VIRTIO_MMIO_QUEUE_USED_HIGH): {
        if (dev->data.QueueSel < dev->num_vqs) {
            struct virtq *virtq = get_current_virtq_by_handler(dev);
            uintptr_t ptr = (uintptr_t)virtq->used;
            ptr |= data;
            virtq->used = (struct virtq_used *)ptr;
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_USED_LOW\n", dev->data.QueueSel, dev->num_vqs);
            success = false;
        }
        break;
    }
    case REG_RANGE(REG_VIRTIO_MMIO_QUEUE_USED_HIGH, REG_VIRTIO_MMIO_CONFIG_GENERATION): {
        if (dev->data.QueueSel < dev->num_vqs) {
            struct virtq *virtq = get_current_virtq_by_handler(dev);
            uintptr_t ptr = (uintptr_t)virtq->used;
            ptr |= (uintptr_t)data << 32;
            virtq->used = (struct virtq_used *)ptr;
            // printf("VIRTIO MMIO|INFO: virtq used 0x%lx\n.", ptr);
        } else {
            LOG_VMM_ERR("invalid virtq index 0x%lx (number of virtqs is 0x%lx) "
                        "given when accessing REG_VIRTIO_MMIO_QUEUE_USED_HIGH\n", dev->data.QueueSel, dev->num_vqs);
            success = false;
        }
        break;
    }
    case REG_RANGE(REG_VIRTIO_MMIO_CONFIG, REG_VIRTIO_MMIO_CONFIG + 0x100):
        success = dev->funs->set_device_config(dev, offset, data);
        break;
    default:
        printf("VIRTIO MMIO|INFO: unknown register 0x%x.", offset);
        success = false;
    }

    return success;
}

bool virtio_mmio_fault_handle(size_t vcpu_id, size_t offset, size_t fsr, seL4_UserContext *regs, void *data)
{
    virtio_device_t *dev = (virtio_device_t *) data;
    assert(dev);
    if (fault_is_read(fsr)) {
        return handle_virtio_mmio_reg_read(dev, vcpu_id, offset, fsr, regs);
    } else {
        return handle_virtio_mmio_reg_write(dev, vcpu_id, offset, fsr, regs);
    }
}

/*
 * If the guest acknowledges the virtual IRQ associated with the virtIO
 * device, there is nothing that we need to do.
 */
static void virtio_virq_default_ack(size_t vcpu_id, int irq, void *cookie) {}

bool virtio_mmio_register_device(virtio_device_t *dev,
                                 uintptr_t region_base,
                                 uintptr_t region_size,
                                 size_t virq)
{
    bool success;
    success = fault_register_vm_exception_handler(region_base,
                                                  region_size,
                                                  &virtio_mmio_fault_handle,
                                                  dev);
    if (!success) {
        LOG_VMM_ERR("Could not register virtual memory fault handler for "
                    "virtIO region [0x%lx..0x%lx)\n", region_base, region_base + region_size);
        return false;
    }

    /* Register the virtual IRQ that will be used to communicate from the device
     * to the guest. This assumes that the interrupt controller is already setup. */
    // @ivanv: we should check that (on AArch64) the virq is an SPI.
    success = virq_register(GUEST_BOOT_VCPU_ID, virq, &virtio_virq_default_ack, NULL);
    assert(success);

    return success;
}
