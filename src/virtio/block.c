/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <microkit.h>
#include <stdint.h>
#include <stdbool.h>
#include <libvmm/virq.h>
#include <libvmm/util/util.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/virtq.h>
#include <libvmm/virtio/mmio.h>
#include <libvmm/virtio/block.h>
#include <sddf/blk/queue.h>
#include <sddf/util/fsmalloc.h>
#include <sddf/util/ialloc.h>

/* Uncomment this to enable debug logging */
// #define DEBUG_BLOCK

#if defined(DEBUG_BLOCK)
#define LOG_BLOCK(...) do{ printf("VIRTIO(BLOCK): "); printf(__VA_ARGS__); }while(0)
#else
#define LOG_BLOCK(...) do{}while(0)
#endif

#define LOG_BLOCK_ERR(...) do{ printf("VIRTIO(BLOCK)|ERROR: "); printf(__VA_ARGS__); }while(0)

static inline struct virtio_blk_device *device_state(struct virtio_device *dev)
{
    return (struct virtio_blk_device *)dev->device_data;
}

static void virtio_blk_mmio_reset(struct virtio_device *dev)
{
    dev->vqs[VIRTIO_BLK_DEFAULT_VIRTQ].ready = false;
    dev->vqs[VIRTIO_BLK_DEFAULT_VIRTQ].last_idx = 0;
}

static bool virtio_blk_mmio_get_device_features(struct virtio_device *dev, uint32_t *features)
{
    if (dev->data.Status & VIRTIO_CONFIG_S_FEATURES_OK) {
        LOG_BLOCK_ERR("driver somehow wants to read device features after FEATURES_OK\n");
    }

    switch (dev->data.DeviceFeaturesSel) {
    /* feature bits 0 to 31 */
    case 0:
        *features = BIT_LOW(VIRTIO_BLK_F_FLUSH);
        *features = *features | BIT_LOW(VIRTIO_BLK_F_BLK_SIZE);
        break;
    /* features bits 32 to 63 */
    case 1:
        *features = BIT_HIGH(VIRTIO_F_VERSION_1);
        break;
    default:
        LOG_BLOCK_ERR("driver sets DeviceFeaturesSel to 0x%x, which doesn't make sense\n",
                      dev->data.DeviceFeaturesSel);
        return false;
    }

    return true;
}

static bool virtio_blk_mmio_set_driver_features(struct virtio_device *dev, uint32_t features)
{
    /* According to virtio initialisation protocol,
       this should check what device features were set, and return the subset of features understood
       by the driver. */
    bool success = false;

    uint32_t device_features = 0;
    device_features = device_features | BIT_LOW(VIRTIO_BLK_F_FLUSH);
    device_features = device_features | BIT_LOW(VIRTIO_BLK_F_BLK_SIZE);

    switch (dev->data.DriverFeaturesSel) {
    /* feature bits 0 to 31 */
    case 0:
        success = (features == device_features);
        break;
    /* features bits 32 to 63 */
    case 1:
        success = (features == BIT_HIGH(VIRTIO_F_VERSION_1));
        break;
    default:
        LOG_BLOCK_ERR("driver sets DriverFeaturesSel to 0x%x, which doesn't make sense\n",
                      dev->data.DriverFeaturesSel);
        return false;
    }

    if (success) {
        dev->data.features_happy = 1;
    }

    return success;
}

static bool virtio_blk_mmio_get_device_config(struct virtio_device *dev, uint32_t offset, uint32_t *ret_val)
{
    struct virtio_blk_device *state = device_state(dev);

    uintptr_t config_base_addr = (uintptr_t)&state->config;
    uintptr_t config_field_offset = (uintptr_t)(offset - REG_VIRTIO_MMIO_CONFIG);
    uint32_t *config_field_addr = (uint32_t *)(config_base_addr + config_field_offset);
    *ret_val = *config_field_addr;
    LOG_BLOCK("get device config with base_addr 0x%x and field_address 0x%x has value %d\n",
              config_base_addr, config_field_addr, *ret_val);

    return true;
}

static bool virtio_blk_mmio_set_device_config(struct virtio_device *dev, uint32_t offset, uint32_t val)
{
    struct virtio_blk_device *state = device_state(dev);

    uintptr_t config_base_addr = (uintptr_t)&state->config;
    uintptr_t config_field_offset = (uintptr_t)(offset - REG_VIRTIO_MMIO_CONFIG);
    uint32_t *config_field_addr = (uint32_t *)(config_base_addr + config_field_offset);
    *config_field_addr = val;
    LOG_BLOCK("set device config with base_addr 0x%x and field_address 0x%x with value %d\n",
              config_base_addr, config_field_addr, val);

    return true;
}

static void virtio_blk_used_buffer(struct virtio_device *dev, uint16_t desc)
{
    struct virtq *virtq = &dev->vqs[VIRTIO_BLK_DEFAULT_VIRTQ].virtq;
    struct virtq_used_elem used_elem = {desc, 0};

    virtq->used->ring[virtq->used->idx % virtq->num] = used_elem;
    virtq->used->idx++;
}

static bool virtio_blk_virq_inject(struct virtio_device *dev)
{
    return virq_inject(dev->virq);
}

static void virtio_blk_set_interrupt_status(struct virtio_device *dev,
                                            bool used_buffer,
                                            bool config_change)
{
    /* Set the reason of the irq.
       bit 0: used buffer
       bit 1: configuration change */
    dev->data.InterruptStatus = used_buffer | (config_change << 1);
}

/* Set response to virtio request to error */
static void virtio_blk_set_req_fail(struct virtio_device *dev, uint16_t desc)
{
    struct virtq *virtq = &dev->vqs[VIRTIO_BLK_DEFAULT_VIRTQ].virtq;

    uint16_t curr_virtio_desc = desc;
    for (; virtq->desc[curr_virtio_desc].flags & VIRTQ_DESC_F_NEXT;
         curr_virtio_desc = virtq->desc[curr_virtio_desc].next) {}
    *((uint8_t *)virtq->desc[curr_virtio_desc].addr) = VIRTIO_BLK_S_IOERR;
}

static void virtio_blk_set_req_success(struct virtio_device *dev, uint16_t desc)
{
    struct virtq *virtq = &dev->vqs[VIRTIO_BLK_DEFAULT_VIRTQ].virtq;

    uint16_t curr_virtio_desc = desc;
    for (; virtq->desc[curr_virtio_desc].flags & VIRTQ_DESC_F_NEXT;
         curr_virtio_desc = virtq->desc[curr_virtio_desc].next) {}
    *((uint8_t *)virtq->desc[curr_virtio_desc].addr) = VIRTIO_BLK_S_OK;
}

static bool sddf_make_req_check(struct virtio_blk_device *state, uint16_t sddf_count)
{
    /* Check if ialloc is full, if data region is full, if req queue is full.
       If these all pass then this request can be handled successfully */
    if (ialloc_full(&state->ialloc)) {
        LOG_BLOCK_ERR("Request bookkeeping array is full\n");
        return false;
    }

    if (blk_queue_full_req(&state->queue_h)) {
        LOG_BLOCK_ERR("Request queue is full\n");
        return false;
    }

    if (fsmalloc_full(&state->fsmalloc, sddf_count)) {
        LOG_BLOCK_ERR("Data region is full\n");
        return false;
    }

    return true;
}

static bool virtio_blk_mmio_queue_notify(struct virtio_device *dev)
{
    /* If multiqueue feature bit negotiated, should read which queue from dev->QueueNotify,
       but for now we just assume it's the one and only default queue */
    virtio_queue_handler_t *vq = &dev->vqs[VIRTIO_BLK_DEFAULT_VIRTQ];
    struct virtq *virtq = &vq->virtq;

    struct virtio_blk_device *state = device_state(dev);

    bool has_dropped = false; /* if any request has to be dropped due to any number of reasons, this becomes true */

    /* get next available request to be handled */
    uint16_t idx = vq->last_idx;

    int err = 0;
    LOG_BLOCK("------------- Driver notified device -------------\n");
    for (; idx != virtq->avail->idx; idx++) {
        uint16_t desc_head = virtq->avail->ring[idx % virtq->num];

        uint16_t curr_desc_head = desc_head;

        /* Print out what the request type is */
        struct virtio_blk_outhdr *virtio_req = (void *)virtq->desc[curr_desc_head].addr;
        LOG_BLOCK("----- Request type is 0x%x -----\n", virtio_req->type);

        /* Parse different requests */
        switch (virtio_req->type) {
        /* There are three parts with each block request. The header, body (which contains the data) and reply. */
        case VIRTIO_BLK_T_IN: {
            LOG_BLOCK("Request type is VIRTIO_BLK_T_IN\n");
            LOG_BLOCK("Sector (read/write offset) is %d\n", virtio_req->sector);

            curr_desc_head = virtq->desc[curr_desc_head].next;
            LOG_BLOCK("Descriptor index is %d, Descriptor flags are: 0x%x, length is 0x%x\n", curr_desc_head,
                      (uint16_t)virtq->desc[curr_desc_head].flags, virtq->desc[curr_desc_head].len);

            /* Converting virtio sector number to sddf block number, we are rounding down */
            uint32_t sddf_block_number = (virtio_req->sector * VIRTIO_BLK_SECTOR_SIZE) / BLK_TRANSFER_SIZE;
            /* Converting bytes to the number of blocks, we are rounding up */
            uint16_t sddf_count = (virtq->desc[curr_desc_head].len + BLK_TRANSFER_SIZE - 1) / BLK_TRANSFER_SIZE;

            if (!sddf_make_req_check(state, sddf_count)) {
                virtio_blk_set_req_fail(dev, desc_head);
                has_dropped = true;
                break;
            }

            /* Allocate data buffer from data region based on sddf_count */
            uintptr_t sddf_data;
            fsmalloc_alloc(&state->fsmalloc, &sddf_data, sddf_count);

            /* Bookkeep the virtio sddf block size translation */
            uintptr_t virtio_data = sddf_data + (virtio_req->sector * VIRTIO_BLK_SECTOR_SIZE) % BLK_TRANSFER_SIZE;
            uintptr_t virtio_data_size = virtq->desc[curr_desc_head].len;

            /* Book keep the request */
            uint32_t req_id;
            ialloc_alloc(&state->ialloc, &req_id);
            state->reqbk[req_id] = (reqbk_t) {
                desc_head, sddf_data, sddf_count, sddf_block_number,
                           virtio_data, virtio_data_size, 0
            };

            uintptr_t offset = sddf_data - ((struct virtio_blk_device *)dev->device_data)->data_region;
            err = blk_enqueue_req(&state->queue_h, BLK_REQ_READ, offset, sddf_block_number, sddf_count, req_id);
            assert(!err);
            break;
        }
        case VIRTIO_BLK_T_OUT: {
            LOG_BLOCK("Request type is VIRTIO_BLK_T_OUT\n");
            LOG_BLOCK("Sector (read/write offset) is %d\n", virtio_req->sector);

            curr_desc_head = virtq->desc[curr_desc_head].next;
            LOG_BLOCK("Descriptor index is %d, Descriptor flags are: 0x%x, length is 0x%x\n", curr_desc_head,
                      (uint16_t)virtq->desc[curr_desc_head].flags, virtq->desc[curr_desc_head].len);

            /* Converting virtio sector number to sddf block number, we are rounding down */
            uint32_t sddf_block_number = (virtio_req->sector * VIRTIO_BLK_SECTOR_SIZE) / BLK_TRANSFER_SIZE;
            /* Converting bytes to the number of blocks, we are rounding up */
            uint16_t sddf_count = (virtq->desc[curr_desc_head].len + BLK_TRANSFER_SIZE - 1) / BLK_TRANSFER_SIZE;

            bool aligned = ((virtio_req->sector % (BLK_TRANSFER_SIZE / VIRTIO_BLK_SECTOR_SIZE)) == 0);

            /* If the write request is not aligned to the sddf transfer size, we need to do a read-modify-write:
            we need to first read the surrounding aligned memory, overwrite that read memory on the unaligned areas
            we want write to, and then write the entire memory back to disk. */
            if (!aligned) {
                if (!sddf_make_req_check(state, sddf_count)) {
                    virtio_blk_set_req_fail(dev, desc_head);
                    has_dropped = true;
                    break;
                }

                /* Allocate data buffer from data region based on sddf_count */
                uintptr_t sddf_data;
                fsmalloc_alloc(&state->fsmalloc, &sddf_data, sddf_count);

                /* Bookkeep the virtio sddf block size translation */
                uintptr_t virtio_data = sddf_data + (virtio_req->sector * VIRTIO_BLK_SECTOR_SIZE) % BLK_TRANSFER_SIZE;
                uintptr_t virtio_data_size = virtq->desc[curr_desc_head].len;

                /* Book keep the request */
                uint32_t req_id;
                ialloc_alloc(&state->ialloc, &req_id);
                state->reqbk[req_id] = (reqbk_t) {
                    desc_head, sddf_data, sddf_count, sddf_block_number,
                               virtio_data, virtio_data_size, aligned
                };

                uintptr_t offset = sddf_data - ((struct virtio_blk_device *)dev->device_data)->data_region;
                err = blk_enqueue_req(&state->queue_h, BLK_REQ_READ, offset, sddf_block_number, sddf_count, req_id);
                assert(!err);
            } else {
                if (!sddf_make_req_check(state, sddf_count)) {
                    virtio_blk_set_req_fail(dev, desc_head);
                    has_dropped = true;
                    break;
                }

                /* Allocate data buffer from data region based on sddf_count */
                uintptr_t sddf_data;
                fsmalloc_alloc(&state->fsmalloc, &sddf_data, sddf_count);

                /* Bookkeep the virtio sddf block size translation */
                uintptr_t virtio_data = sddf_data + (virtio_req->sector * VIRTIO_BLK_SECTOR_SIZE) % BLK_TRANSFER_SIZE;
                uintptr_t virtio_data_size = virtq->desc[curr_desc_head].len;

                /* Book keep the request */
                uint32_t req_id;
                ialloc_alloc(&state->ialloc, &req_id);
                state->reqbk[req_id] = (reqbk_t) {
                    desc_head, sddf_data, sddf_count, sddf_block_number,
                               virtio_data, virtio_data_size, aligned
                };

                /* Copy data from virtio buffer to data buffer, create sddf write request and initialise it with data buffer */
                memcpy((void *)sddf_data, (void *)virtq->desc[curr_desc_head].addr, virtq->desc[curr_desc_head].len);

                uintptr_t offset = sddf_data - ((struct virtio_blk_device *)dev->device_data)->data_region;
                err = blk_enqueue_req(&state->queue_h, BLK_REQ_WRITE, offset, sddf_block_number, sddf_count, req_id);
                assert(!err);
            }
            break;
        }
        case VIRTIO_BLK_T_FLUSH: {
            LOG_BLOCK("Request type is VIRTIO_BLK_T_FLUSH\n");

            if (!sddf_make_req_check(state, 0)) {
                virtio_blk_set_req_fail(dev, desc_head);
                has_dropped = true;
                break;
            }

            /* Book keep the request */
            uint32_t req_id;
            ialloc_alloc(&state->ialloc, &req_id);
            /* except for virtio desc, nothing else needs to be retrieved later
             * so leave as 0 */
            state->reqbk[req_id] = (reqbk_t) {
                desc_head, 0, 0, 0, 0, 0
            };

            err = blk_enqueue_req(&state->queue_h, BLK_REQ_FLUSH, 0, 0, 0, req_id);
            break;
        }
        default: {
            LOG_BLOCK_ERR(
                "Handling VirtIO block request, but virtIO request type is not recognised: %d\n",
                virtio_req->type);
            virtio_blk_set_req_fail(dev, desc_head);
            has_dropped = true;
            break;
        }
        }
    }

    /* Update virtq index to the next available request to be handled */
    vq->last_idx = idx;

    bool success = true;

    /* If any request has to be dropped due to any number of reasons, we inject an interrupt */
    if (has_dropped) {
        virtio_blk_set_interrupt_status(dev, true, false);
        success = virtio_blk_virq_inject(dev);
    }

    if (!blk_queue_plugged_req(&state->queue_h)) {
        /* there is a world where all requests to be handled during this batch
         * are dropped and hence this notify to the other PD would be redundant */
        microkit_notify(state->server_ch);
    }

    return success;
}

bool virtio_blk_handle_resp(struct virtio_blk_device *state)
{
    struct virtio_device *dev = &state->virtio_device;

    blk_resp_status_t sddf_ret_status;
    uint16_t sddf_ret_success_count;
    uint32_t sddf_ret_id;

    bool handled = false;
    int err = 0;
    while (!blk_queue_empty_resp(&state->queue_h)) {
        err = blk_dequeue_resp(&state->queue_h,
                               &sddf_ret_status,
                               &sddf_ret_success_count,
                               &sddf_ret_id);
        assert(!err);

        /* Freeing and retrieving data store */
        reqbk_t *data = &state->reqbk[sddf_ret_id];
        ialloc_free(&state->ialloc, sddf_ret_id);

        struct virtq *virtq = &dev->vqs[VIRTIO_BLK_DEFAULT_VIRTQ].virtq;

        struct virtio_blk_outhdr *virtio_req = (void *)virtq->desc[data->virtio_desc_head].addr;

        uint16_t curr_virtio_desc = virtq->desc[data->virtio_desc_head].next;

        bool resp_success = false;
        if (sddf_ret_status == BLK_RESP_OK) {
            resp_success = true;
            switch (virtio_req->type) {
            case VIRTIO_BLK_T_IN: {
                memcpy((void *)virtq->desc[curr_virtio_desc].addr,
                       (void *)data->virtio_data, data->virtio_data_size);
                break;
            }
            case VIRTIO_BLK_T_OUT: {
                if (!data->aligned) {
                    /* Copy the write data into an offset into the allocated sddf data buffer */
                    memcpy((void *)data->virtio_data,
                           (void *)virtq->desc[curr_virtio_desc].addr,
                           data->virtio_data_size);

                    uint32_t new_sddf_id;
                    ialloc_alloc(&state->ialloc, &new_sddf_id);
                    state->reqbk[new_sddf_id] = (reqbk_t) {
                        data->virtio_desc_head,
                             data->sddf_data, data->sddf_count,
                             data->sddf_block_number, 0, 0, true
                    };

                    err = blk_enqueue_req(&state->queue_h,
                                          BLK_REQ_WRITE,
                                          data->sddf_data - state->data_region,
                                          data->sddf_block_number,
                                          data->sddf_count,
                                          new_sddf_id);
                    assert(!err);
                    microkit_notify(state->server_ch);
                    continue;
                }
                break;
            }
            case VIRTIO_BLK_T_FLUSH:
                break;
            default: {
                LOG_BLOCK_ERR(
                    "Retrieving sDDF block response, but virtIO request type is not recognised: %d\n",
                    virtio_req->type);
                resp_success = false;
                break;
            }
            }
        }

        if (resp_success) {
            virtio_blk_set_req_success(dev, data->virtio_desc_head);
        } else {
            virtio_blk_set_req_fail(dev, data->virtio_desc_head);
        }

        /* Free corresponding bookkeeping structures regardless of the request's
         * success status */
        if (virtio_req->type == VIRTIO_BLK_T_IN || virtio_req->type == VIRTIO_BLK_T_OUT) {
            fsmalloc_free(&state->fsmalloc, data->sddf_data, data->sddf_count);
        }

        virtio_blk_used_buffer(dev, data->virtio_desc_head);

        handled = true;
    }

    bool success = true;

    /* We need to know if we handled any responses, if we did we inject an
     * interrupt, if we didn't we don't inject */
    if (handled) {
        virtio_blk_set_interrupt_status(dev, true, false);
        success = virtio_blk_virq_inject(dev);
    }

    return success;
}

static void virtio_blk_config_init(struct virtio_blk_device *blk_dev)
{
    blk_storage_info_t *storage_info = blk_dev->storage_info;

    blk_dev->config.capacity = (BLK_TRANSFER_SIZE / VIRTIO_BLK_SECTOR_SIZE) * storage_info->capacity;
    if (storage_info->block_size != 0) {
        blk_dev->config.blk_size = storage_info->block_size * BLK_TRANSFER_SIZE;
    } else {
        blk_dev->config.blk_size = storage_info->sector_size;
    }
}

static virtio_device_funs_t functions = {
    .device_reset = virtio_blk_mmio_reset,
    .get_device_features = virtio_blk_mmio_get_device_features,
    .set_driver_features = virtio_blk_mmio_set_driver_features,
    .get_device_config = virtio_blk_mmio_get_device_config,
    .set_device_config = virtio_blk_mmio_set_device_config,
    .queue_notify = virtio_blk_mmio_queue_notify,
};

bool virtio_mmio_blk_init(struct virtio_blk_device *blk_dev,
                          uintptr_t region_base,
                          uintptr_t region_size,
                          size_t virq,
                          uintptr_t data_region,
                          size_t data_region_size,
                          blk_storage_info_t *storage_info,
                          blk_queue_handle_t *queue_h,
                          int server_ch)
{
    struct virtio_device *dev = &blk_dev->virtio_device;

    dev->data.DeviceID = DEVICE_ID_VIRTIO_BLOCK;
    dev->data.VendorID = VIRTIO_MMIO_DEV_VENDOR_ID;
    dev->funs = &functions;
    dev->vqs = blk_dev->vqs;
    dev->num_vqs = VIRTIO_BLK_NUM_VIRTQ;
    dev->virq = virq;
    dev->device_data = blk_dev;

    blk_dev->storage_info = storage_info;
    blk_dev->queue_h = *queue_h;
    blk_dev->data_region = data_region;
    blk_dev->server_ch = server_ch;

    size_t sddf_data_buffers = data_region_size / BLK_TRANSFER_SIZE;
    /* This assert is necessary as the bookkeeping data structures need to have a
     * defined size at compile time and that depends on the number of buffers
     * passed to us during initialisation. */
    assert(sddf_data_buffers <= SDDF_MAX_DATA_BUFFERS);

    virtio_blk_config_init(blk_dev);

    fsmalloc_init(&blk_dev->fsmalloc,
                  data_region,
                  BLK_TRANSFER_SIZE,
                  sddf_data_buffers,
                  &blk_dev->fsmalloc_avail_bitarr,
                  blk_dev->fsmalloc_avail_bitarr_words,
                  roundup_bits2words64(sddf_data_buffers));

    ialloc_init(&blk_dev->ialloc, blk_dev->ialloc_idxlist, sddf_data_buffers);

    return virtio_mmio_register_device(dev, region_base, region_size, virq);
}