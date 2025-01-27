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

static inline void virtio_blk_mmio_reset(struct virtio_device *dev) {
  dev->vqs[VIRTIO_BLK_DEFAULT_VIRTQ].ready = false;
  dev->vqs[VIRTIO_BLK_DEFAULT_VIRTQ].last_idx = 0;
}

static inline bool
virtio_blk_mmio_get_device_features(struct virtio_device *dev,
                                    uint32_t *features) {
  if (dev->data.Status & VIRTIO_CONFIG_S_FEATURES_OK) {
    LOG_BLOCK_ERR(
        "driver somehow wants to read device features after FEATURES_OK\n");
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
    LOG_BLOCK_ERR(
        "driver sets DeviceFeaturesSel to 0x%x, which doesn't make sense\n",
        dev->data.DeviceFeaturesSel);
    return false;
  }

  return true;
}

static inline bool
virtio_blk_mmio_set_driver_features(struct virtio_device *dev,
                                    uint32_t features) {
  /* According to virtio initialisation protocol,
     this should check what device features were set, and return the subset of
     features understood by the driver. */
  bool success = false;

  uint32_t device_features = 0;
  device_features |= BIT_LOW(VIRTIO_BLK_F_FLUSH);
  device_features |= BIT_LOW(VIRTIO_BLK_F_BLK_SIZE);

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
    LOG_BLOCK_ERR(
        "driver sets DriverFeaturesSel to 0x%x, which doesn't make sense\n",
        dev->data.DriverFeaturesSel);
    return false;
  }

  if (success) {
    dev->data.features_happy = 1;
  }

  return success;
}

static inline bool virtio_blk_mmio_get_device_config(struct virtio_device *dev,
                                                     uint32_t offset,
                                                     uint32_t *ret_val) {
  struct virtio_blk_device *state = device_state(dev);

  uintptr_t config_base_addr = (uintptr_t)&state->config;
  uintptr_t config_field_offset = (uintptr_t)(offset - REG_VIRTIO_MMIO_CONFIG);
  uint32_t *config_field_addr =
      (uint32_t *)(config_base_addr + config_field_offset);
  *ret_val = *config_field_addr;
  LOG_BLOCK("get device config with base_addr 0x%x and field_address 0x%x has "
            "value %d\n",
            config_base_addr, config_field_addr, *ret_val);

  return true;
}

static inline bool virtio_blk_mmio_set_device_config(struct virtio_device *dev,
                                                     uint32_t offset,
                                                     uint32_t val) {
  struct virtio_blk_device *state = device_state(dev);

  uintptr_t config_base_addr = (uintptr_t)&state->config;
  uintptr_t config_field_offset = (uintptr_t)(offset - REG_VIRTIO_MMIO_CONFIG);
  uint32_t *config_field_addr =
      (uint32_t *)(config_base_addr + config_field_offset);
  *config_field_addr = val;
  LOG_BLOCK("set device config with base_addr 0x%x and field_address 0x%x with "
            "value %d\n",
            config_base_addr, config_field_addr, val);

  return true;
}

static inline void virtio_blk_used_buffer(struct virtio_device *dev,
                                          uint16_t desc) {
  struct virtq *virtq = &dev->vqs[VIRTIO_BLK_DEFAULT_VIRTQ].virtq;
  struct virtq_used_elem used_elem = {desc, 0};

  virtq->used->ring[virtq->used->idx % virtq->num] = used_elem;
  virtq->used->idx++;
}

static inline bool virtio_blk_virq_inject(struct virtio_device *dev) {
  return virq_inject(GUEST_VCPU_ID, dev->virq);
}

static inline void virtio_blk_set_interrupt_status(struct virtio_device *dev,
                                                   bool used_buffer,
                                                   bool config_change) {
  /* Set the reason of the irq.
     bit 0: used buffer
     bit 1: configuration change */
  dev->data.InterruptStatus = used_buffer | (config_change << 1);
}

static inline void virtio_blk_set_req_fail(struct virtio_device *dev,
                                           uint16_t desc) {
  struct virtq *virtq = &dev->vqs[VIRTIO_BLK_DEFAULT_VIRTQ].virtq;

  /* Loop to the final byte of the final descriptor and write response code
   * there */
  uint16_t curr_desc = desc;
  while (virtq->desc[curr_desc].flags & VIRTQ_DESC_F_NEXT) {
    curr_desc = virtq->desc[curr_desc].next;
  }
  assert(virtq->desc[curr_desc].flags & VIRTQ_DESC_F_WRITE);
  *(uint8_t *)(virtq->desc[curr_desc].addr + virtq->desc[curr_desc].len - 1) =
      VIRTIO_BLK_S_IOERR;
}

static inline void virtio_blk_set_req_success(struct virtio_device *dev,
                                              uint16_t desc) {
  struct virtq *virtq = &dev->vqs[VIRTIO_BLK_DEFAULT_VIRTQ].virtq;

  uint16_t curr_desc = desc;
  while (virtq->desc[curr_desc].flags & VIRTQ_DESC_F_NEXT) {
    curr_desc = virtq->desc[curr_desc].next;
  }
  assert(virtq->desc[curr_desc].flags & VIRTQ_DESC_F_WRITE);
  *((uint8_t *)virtq->desc[curr_desc].addr) = VIRTIO_BLK_S_OK;
}

static inline bool sddf_make_req_check(struct virtio_blk_device *state,
                                       uint16_t sddf_count) {
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
  int err = 0;
  /* If multiqueue feature bit negotiated, should read which queue from
     dev->QueueNotify, but for now we just assume it's the one and only default
     queue */
  virtio_queue_handler_t *vq = &dev->vqs[VIRTIO_BLK_DEFAULT_VIRTQ];
  struct virtq *virtq = &vq->virtq;

  struct virtio_blk_device *state = device_state(dev);

  /* If any request has to be dropped due to any number of reasons, this becomes
   * true */
  bool has_dropped = false;

  bool virt_notify = false;

  /* Handle available requests beginning from the last handled request */
  uint16_t last_handled_avail_idx = vq->last_idx;

  LOG_BLOCK("------------- Driver notified device -------------\n");
  for (; last_handled_avail_idx != virtq->avail->idx;
       last_handled_avail_idx++) {
    uint16_t desc_head =
        virtq->avail->ring[last_handled_avail_idx % virtq->num];
    uint16_t curr_desc = desc_head;
    uint32_t curr_desc_bytes_read = 0;

    /* There are three parts with each block request. The header, body (which
     * contains the data) and reply. */
    uint32_t header_bytes_read = 0;
    struct virtio_blk_outhdr virtio_req_header;
    for (; header_bytes_read < sizeof(struct virtio_blk_outhdr);
         curr_desc = virtq->desc[curr_desc].next) {
      /* Header is device read only */
      assert(!(virtq->desc[curr_desc].flags & VIRTQ_DESC_F_WRITE));
      /* We can guarantee existence of next descriptor as footer is write only
       */
      assert(virtq->desc[curr_desc].flags & VIRTQ_DESC_F_NEXT);
      if (header_bytes_read + virtq->desc[curr_desc].len >
          sizeof(struct virtio_blk_outhdr)) {
        memcpy(&virtio_req_header, (void *)virtq->desc[curr_desc].addr,
               sizeof(struct virtio_blk_outhdr) - header_bytes_read);
        curr_desc_bytes_read =
            sizeof(struct virtio_blk_outhdr) - header_bytes_read;
        header_bytes_read +=
            sizeof(struct virtio_blk_outhdr) - header_bytes_read;
        /* Don't go to the next descriptor yet, we're not done processing with
         * current one */
        break;
      } else {
        memcpy(&virtio_req_header, (void *)virtq->desc[curr_desc].addr,
               virtq->desc[curr_desc].len);
        header_bytes_read += virtq->desc[curr_desc].len;
      }
    }

    LOG_BLOCK("----- Request type is 0x%x -----\n", virtio_req_header.type);

    switch (virtio_req_header.type) {
    case VIRTIO_BLK_T_IN: {
      LOG_BLOCK("Request type is VIRTIO_BLK_T_IN\n");
      LOG_BLOCK("Sector (read/write offset) is %d\n", virtio_req_header.sector);

      /* Converting virtio sector number to sddf block number, we are rounding
       * down */
      uint32_t sddf_block_number =
          (virtio_req_header.sector * VIRTIO_BLK_SECTOR_SIZE) /
          BLK_TRANSFER_SIZE;

      /* Figure out how many bytes are in the body of the request */
      uint32_t body_size_bytes = 0;
      uint32_t tmp_curr_desc_bytes_read = curr_desc_bytes_read;
      for (uint16_t tmp_curr_desc = curr_desc;
           virtq->desc[tmp_curr_desc].flags & VIRTQ_DESC_F_NEXT;
           tmp_curr_desc = virtq->desc[tmp_curr_desc].next) {
        if (tmp_curr_desc_bytes_read != 0) {
          body_size_bytes +=
              virtq->desc[tmp_curr_desc].len - tmp_curr_desc_bytes_read;
          tmp_curr_desc_bytes_read = 0;
        } else {
          body_size_bytes += virtq->desc[tmp_curr_desc].len;
        }
        if (!(virtq->desc[tmp_curr_desc].flags & VIRTQ_DESC_F_WRITE) ||
            virtq->desc[tmp_curr_desc].len < VIRTIO_BLK_SECTOR_SIZE) {
          break;
        }
      }

      /* Converting bytes to the number of blocks, we are rounding up */
      uint16_t sddf_count =
          (body_size_bytes + BLK_TRANSFER_SIZE - 1) / BLK_TRANSFER_SIZE;

      if (!sddf_make_req_check(state, sddf_count)) {
        virtio_blk_set_req_fail(dev, curr_desc);
        has_dropped = true;
        break;
      }

      /* Allocate data cells from sddf data region based on sddf_count */
      uintptr_t sddf_data_cell_base;
      fsmalloc_alloc(&state->fsmalloc, &sddf_data_cell_base, sddf_count);

      /* Find address within the data cells for reading/writing virtio data */
      uintptr_t sddf_data = sddf_data_cell_base + (virtio_req_header.sector *
                                                   VIRTIO_BLK_SECTOR_SIZE) %
                                                      BLK_TRANSFER_SIZE;

      /* Generate sddf request id and bookkeep the request */
      uint32_t req_id;
      err = ialloc_alloc(&state->ialloc, &req_id);
      assert(!err);
      state->reqsbk[req_id] = (reqbk_t){
          desc_head, sddf_data_cell_base, sddf_count, sddf_block_number,
          sddf_data, body_size_bytes,     false};

      uintptr_t sddf_offset =
          sddf_data_cell_base -
          ((struct virtio_blk_device *)dev->device_data)->data_region;
      err = blk_enqueue_req(&state->queue_h, BLK_REQ_READ, sddf_offset,
                            sddf_block_number, sddf_count, req_id);
      assert(!err);
      virt_notify = true;
      break;
    }
    case VIRTIO_BLK_T_OUT: {
      LOG_BLOCK("Request type is VIRTIO_BLK_T_OUT\n");
      LOG_BLOCK("Sector (read/write offset) is %d\n", virtio_req_header.sector);

      /* Converting virtio sector number to sddf block number, we are rounding
       * down */
      uint32_t sddf_block_number =
          (virtio_req_header.sector * VIRTIO_BLK_SECTOR_SIZE) /
          BLK_TRANSFER_SIZE;

      /* Figure out how many bytes are in the body of the request */
      uint32_t body_size_bytes = 0;
      uint32_t tmp_curr_desc_bytes_read = curr_desc_bytes_read;
      for (uint16_t tmp_curr_desc = curr_desc;
           virtq->desc[tmp_curr_desc].flags & VIRTQ_DESC_F_NEXT;
           tmp_curr_desc = virtq->desc[tmp_curr_desc].next) {
        if (tmp_curr_desc_bytes_read != 0) {
          body_size_bytes +=
              virtq->desc[tmp_curr_desc].len - tmp_curr_desc_bytes_read;
          tmp_curr_desc_bytes_read = 0;
        } else {
          body_size_bytes += virtq->desc[tmp_curr_desc].len;
        }
        if (!(virtq->desc[tmp_curr_desc].flags & VIRTQ_DESC_F_WRITE) ||
            virtq->desc[tmp_curr_desc].len < VIRTIO_BLK_SECTOR_SIZE) {
          break;
        }
      }

      /* Converting bytes to the number of blocks, we are rounding up */
      uint16_t sddf_count =
          (body_size_bytes + BLK_TRANSFER_SIZE - 1) / BLK_TRANSFER_SIZE;

      if (!sddf_make_req_check(state, sddf_count)) {
        virtio_blk_set_req_fail(dev, curr_desc);
        has_dropped = true;
        break;
      }

      /* If the write request is not aligned on the sddf transfer size, we need
       * to do a read-modify-write: we need to first read the surrounding
       * memory, overwrite the memory on the unaligned areas, and then write the
       * entire memory back to disk.
       */
      bool aligned = ((virtio_req_header.sector %
                       (BLK_TRANSFER_SIZE / VIRTIO_BLK_SECTOR_SIZE)) == 0);
      if (!aligned) {
        /* Allocate data buffer from data region based on sddf_count */
        uintptr_t sddf_data_cell_base;
        fsmalloc_alloc(&state->fsmalloc, &sddf_data_cell_base, sddf_count);
        /* Find address within the data cells for reading/writing virtio data */
        uintptr_t sddf_data = sddf_data_cell_base + (virtio_req_header.sector *
                                                     VIRTIO_BLK_SECTOR_SIZE) %
                                                        BLK_TRANSFER_SIZE;
        /* Generate sddf request id and bookkeep the request */
        uint32_t req_id;
        ialloc_alloc(&state->ialloc, &req_id);
        state->reqsbk[req_id] = (reqbk_t){
            desc_head, sddf_data_cell_base, sddf_count, sddf_block_number,
            sddf_data, body_size_bytes,     aligned};

        uintptr_t sddf_offset =
            sddf_data_cell_base -
            ((struct virtio_blk_device *)dev->device_data)->data_region;
        err = blk_enqueue_req(&state->queue_h, BLK_REQ_READ, sddf_offset,
                              sddf_block_number, sddf_count, req_id);
        assert(!err);
      } else {
        /* Handle normal write request */
        /* Allocate data buffer from data region based on sddf_count */
        uintptr_t sddf_data_cell_base;
        fsmalloc_alloc(&state->fsmalloc, &sddf_data_cell_base, sddf_count);
        /* Find address within the data cells for reading/writing virtio data */
        uintptr_t sddf_data = sddf_data_cell_base + (virtio_req_header.sector *
                                                     VIRTIO_BLK_SECTOR_SIZE) %
                                                        BLK_TRANSFER_SIZE;
        /* Copy data from virtio buffer to sddf buffer */
        uint32_t body_bytes_read = 0;
        for (; body_bytes_read < body_size_bytes;
             curr_desc = virtq->desc[curr_desc].next) {
          /* For write requests, the body is a read descriptor, and the footer
           * is a write descriptor, we know there must be a descriptor cut-off
           * at the end.
           */
          assert(body_bytes_read + virtq->desc[curr_desc].len <=
                 body_size_bytes);
          assert(virtq->desc[curr_desc].flags & VIRTQ_DESC_F_NEXT);
          if (curr_desc_bytes_read != 0) {
            memcpy((void *)sddf_data + body_bytes_read,
                   (void *)virtq->desc[curr_desc].addr + curr_desc_bytes_read,
                   virtq->desc[curr_desc].len - curr_desc_bytes_read);
            body_bytes_read +=
                virtq->desc[curr_desc].len - curr_desc_bytes_read;
            curr_desc_bytes_read = 0;
          } else {
            memcpy((void *)sddf_data + body_bytes_read,
                   (void *)virtq->desc[curr_desc].addr,
                   virtq->desc[curr_desc].len);
            body_bytes_read += virtq->desc[curr_desc].len;
          }
        }

        /* Generate sddf request id and bookkeep the request */
        uint32_t req_id;
        ialloc_alloc(&state->ialloc, &req_id);
        state->reqsbk[req_id] = (reqbk_t){
            desc_head, sddf_data_cell_base, sddf_count, sddf_block_number,
            sddf_data, body_size_bytes,     aligned};

        uintptr_t sddf_offset =
            sddf_data_cell_base -
            ((struct virtio_blk_device *)dev->device_data)->data_region;
        err = blk_enqueue_req(&state->queue_h, BLK_REQ_WRITE, sddf_offset,
                              sddf_block_number, sddf_count, req_id);
        assert(!err);
      }
      virt_notify = true;
      break;
    }
    case VIRTIO_BLK_T_FLUSH: {
      LOG_BLOCK("Request type is VIRTIO_BLK_T_FLUSH\n");

      if (!sddf_make_req_check(state, 0)) {
        virtio_blk_set_req_fail(dev, curr_desc);
        has_dropped = true;
        break;
      }

      /* Bookkeep the request */
      uint32_t req_id;
      ialloc_alloc(&state->ialloc, &req_id);
      /* except for virtio desc, nothing else needs to be retrieved later
       * so leave as 0 */
      state->reqsbk[req_id] = (reqbk_t){desc_head, 0, 0, 0, 0, 0, false};

      err = blk_enqueue_req(&state->queue_h, BLK_REQ_FLUSH, 0, 0, 0, req_id);
      break;
      virt_notify = true;
    }
    default: {
      LOG_BLOCK_ERR("Handling VirtIO block request, but virtIO request type is "
                    "not recognised: %d\n",
                    virtio_req_header.type);
      virtio_blk_set_req_fail(dev, curr_desc);
      has_dropped = true;
      break;
    }
    }
  }

  /* Update virtq index to the next available request to be handled */
  vq->last_idx = last_handled_avail_idx;

  /* If any request has to be dropped due to any number of reasons, we inject an
   * interrupt */
  bool virq_inject_success = true;
  if (has_dropped) {
    virtio_blk_set_interrupt_status(dev, true, false);
    virq_inject_success = virtio_blk_virq_inject(dev);
  }

  if (virt_notify && !blk_queue_plugged_req(&state->queue_h)) {
    microkit_notify(state->server_ch);
  }

  return virq_inject_success;
}

bool virtio_blk_handle_resp(struct virtio_blk_device *state)
{
  int err = 0;
  struct virtio_device *dev = &state->virtio_device;

  blk_resp_status_t sddf_ret_status;
  uint16_t sddf_ret_success_count;
  uint32_t sddf_ret_id;

  bool virt_notify = false;
  bool resp_handled = false;
  while (!blk_queue_empty_resp(&state->queue_h)) {
    err = blk_dequeue_resp(&state->queue_h, &sddf_ret_status,
                           &sddf_ret_success_count, &sddf_ret_id);
    assert(!err);

    /* Retrieve request bookkeep information and free allocated id */
    reqbk_t *reqbk = &state->reqsbk[sddf_ret_id];
    err = ialloc_free(&state->ialloc, sddf_ret_id);
    assert(!err);

    struct virtq *virtq = &dev->vqs[VIRTIO_BLK_DEFAULT_VIRTQ].virtq;

    uint16_t curr_desc = reqbk->virtio_desc_head;
    uint32_t curr_desc_bytes_read = 0;

    uint32_t header_bytes_read = 0;
    struct virtio_blk_outhdr virtio_req_header;
    for (; header_bytes_read < sizeof(struct virtio_blk_outhdr);
         curr_desc = virtq->desc[curr_desc].next) {
      /* Header is device read only */
      assert(!(virtq->desc[curr_desc].flags & VIRTQ_DESC_F_WRITE));
      /* We can always guarantee existence of next descriptor as footer is write
       * only */
      assert(virtq->desc[curr_desc].flags & VIRTQ_DESC_F_NEXT);
      if (header_bytes_read + virtq->desc[curr_desc].len >
          sizeof(struct virtio_blk_outhdr)) {
        memcpy(&virtio_req_header, (void *)virtq->desc[curr_desc].addr,
               sizeof(struct virtio_blk_outhdr) - header_bytes_read);
        curr_desc_bytes_read =
            sizeof(struct virtio_blk_outhdr) - header_bytes_read;
        header_bytes_read +=
            sizeof(struct virtio_blk_outhdr) - header_bytes_read;
        /* Don't go to the next descriptor yet, we're not done processing with
         * current one */
        break;
      } else {
        memcpy(&virtio_req_header, (void *)virtq->desc[curr_desc].addr,
               virtq->desc[curr_desc].len);
        header_bytes_read += virtq->desc[curr_desc].len;
      }
    }

    bool resp_success = false;
    if (sddf_ret_status == BLK_RESP_OK) {
      resp_success = true;
      switch (virtio_req_header.type) {
      case VIRTIO_BLK_T_IN: {
        /* Going from read (header) to write (body) descriptor, there should be
         * a descriptor cut-off at the beginning. */
        assert(curr_desc_bytes_read == 0);
        uint32_t body_bytes_read = 0;
        for (; body_bytes_read < reqbk->virtio_body_size_bytes;
             curr_desc = virtq->desc[curr_desc].next) {
          if (body_bytes_read + virtq->desc[curr_desc].len >
              reqbk->virtio_body_size_bytes) {
            memcpy((void *)virtq->desc[curr_desc].addr,
                   (void *)reqbk->sddf_data + body_bytes_read,
                   reqbk->virtio_body_size_bytes - body_bytes_read);
            body_bytes_read += reqbk->virtio_body_size_bytes - body_bytes_read;
            /* This is the final descriptor if we get into this condition, don't
             * go to next descriptor */
            LOG_VMM("virtq->desc[curr_desc].len: %d\n",
                    virtq->desc[curr_desc].len);
            assert(!(virtq->desc[curr_desc].flags & VIRTQ_DESC_F_NEXT));
            break;
          } else {
            memcpy((void *)virtq->desc[curr_desc].addr,
                   (void *)reqbk->sddf_data + body_bytes_read,
                   virtq->desc[curr_desc].len);
            body_bytes_read += virtq->desc[curr_desc].len;
            /* Because there is still the footer, we are guaranteed next
             * descriptor exists */
            assert(virtq->desc[curr_desc].flags & VIRTQ_DESC_F_NEXT);
          }
        }
        break;
      }
      case VIRTIO_BLK_T_OUT: {
        if (!reqbk->aligned) {
          /* Handling read-modify-write procedure, copy virtio write data to the
           * correct offset in the same sddf data region allocated to do the
           * surrounding read.
           */
          uint32_t body_bytes_read = 0;
          for (; body_bytes_read < reqbk->virtio_body_size_bytes;
               curr_desc = virtq->desc[curr_desc].next) {
            /* For write requests, the body is a read descriptor and the footer
             * is a write descriptor, there must be a descriptor cut-off at the
             * end
             */
            assert(body_bytes_read + virtq->desc[curr_desc].len <=
                   reqbk->virtio_body_size_bytes);
            assert(virtq->desc[curr_desc].flags & VIRTQ_DESC_F_NEXT);
            if (curr_desc_bytes_read != 0) {
              memcpy((void *)reqbk->sddf_data + body_bytes_read,
                     (void *)virtq->desc[curr_desc].addr + curr_desc_bytes_read,
                     virtq->desc[curr_desc].len - curr_desc_bytes_read);
              body_bytes_read +=
                  virtq->desc[curr_desc].len - curr_desc_bytes_read;
              curr_desc_bytes_read = 0;
            } else {
              memcpy((void *)reqbk->sddf_data + body_bytes_read,
                     (void *)virtq->desc[curr_desc].addr,
                     virtq->desc[curr_desc].len);
              body_bytes_read += virtq->desc[curr_desc].len;
            }
          }

          uint32_t new_sddf_id;
          err = ialloc_alloc(&state->ialloc, &new_sddf_id);
          assert(!err);
          state->reqsbk[new_sddf_id] = (reqbk_t){
              reqbk->virtio_desc_head,
              reqbk->sddf_data_cell_base,
              reqbk->sddf_count,
              reqbk->sddf_block_number,
              0, /* unused */
              0, /* unused */
              true,
          };

          err = blk_enqueue_req(&state->queue_h, BLK_REQ_WRITE,
                                reqbk->sddf_data_cell_base - state->data_region,
                                reqbk->sddf_block_number, reqbk->sddf_count,
                                new_sddf_id);
          assert(!err);
          virt_notify = true;
          /* The virtIO request is not complete yet so we don't tell the driver
           * (just skip over to next request) */
          continue;
        }
        break;
      }
      case VIRTIO_BLK_T_FLUSH:
        break;
      default: {
        LOG_BLOCK_ERR("Retrieving sDDF block response, but virtIO request type "
                      "is not recognised: %d\n",
                      virtio_req_header.type);
        resp_success = false;
        break;
      }
      }
    }

    if (resp_success) {
      virtio_blk_set_req_success(dev, curr_desc);
    } else {
      virtio_blk_set_req_fail(dev, curr_desc);
    }

    /* Free corresponding bookkeeping structures regardless of the request's
     * success status.
     */
    if (virtio_req_header.type == VIRTIO_BLK_T_IN ||
        virtio_req_header.type == VIRTIO_BLK_T_OUT) {
      fsmalloc_free(&state->fsmalloc, reqbk->sddf_data_cell_base,
                    reqbk->sddf_count);
    }

    virtio_blk_used_buffer(dev, reqbk->virtio_desc_head);

    resp_handled = true;
  }

  /* We need to know if we handled any responses, if we did, we inject an
   * interrupt, if we didn't we don't inject.
   */
  bool virq_inject_success = true;
  if (resp_handled) {
    virtio_blk_set_interrupt_status(dev, true, false);
    virq_inject_success = virtio_blk_virq_inject(dev);
  }

  if (virt_notify) {
    microkit_notify(state->server_ch);
  }

  return virq_inject_success;
}

static inline void virtio_blk_config_init(struct virtio_blk_device *blk_dev) {
  blk_storage_info_t *storage_info = blk_dev->storage_info;

  blk_dev->config.capacity =
      (BLK_TRANSFER_SIZE / VIRTIO_BLK_SECTOR_SIZE) * storage_info->capacity;
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
                          uintptr_t region_base, uintptr_t region_size,
                          size_t virq, uintptr_t data_region,
                          size_t data_region_size,
                          blk_storage_info_t *storage_info,
                          blk_queue_handle_t *queue_h, uint32_t queue_capacity,
                          int server_ch) {
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
  blk_dev->queue_capacity = queue_capacity;
  blk_dev->server_ch = server_ch;

  size_t num_sddf_cells =
      (data_region_size / BLK_TRANSFER_SIZE) < SDDF_MAX_DATA_CELLS
          ? (data_region_size / BLK_TRANSFER_SIZE)
          : SDDF_MAX_DATA_CELLS;

  virtio_blk_config_init(blk_dev);

  fsmalloc_init(&blk_dev->fsmalloc, data_region, BLK_TRANSFER_SIZE,
                num_sddf_cells, &blk_dev->fsmalloc_avail_bitarr,
                blk_dev->fsmalloc_avail_bitarr_words,
                roundup_bits2words64(num_sddf_cells));

  ialloc_init(&blk_dev->ialloc, blk_dev->ialloc_idxlist, num_sddf_cells);

  return virtio_mmio_register_device(dev, region_base, region_size, virq);
}
