/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <assert.h>
#include <blk_config.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sddf/blk/queue.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <uio/blk.h>
#include <uio/libuio.h>
#include <unistd.h>

/* Uncomment this to enable debug logging */
// #define DEBUG_UIO_BLOCK

#if defined(DEBUG_UIO_BLOCK)
#define LOG_UIO_BLOCK(...)                                                     \
  do {                                                                         \
    printf("UIO_DRIVER(BLOCK)");                                               \
    printf("|INFO: ");                                                         \
    printf(__VA_ARGS__);                                                       \
  } while (0)
#else
#define LOG_UIO_BLOCK(...) do{}while(0)
#endif

#define LOG_UIO_BLOCK_ERR(...) do{ printf("UIO_DRIVER(BLOCK)"); printf("|ERROR: "); printf(__VA_ARGS__); }while(0)

#define _unused(x) ((void)(x))

#define STORAGE_MAX_PATHNAME 64

int storage_fd;

blk_storage_info_t *blk_storage_info;
blk_queue_handle_t h;
uintptr_t blk_client_data[BLK_NUM_CLIENTS + 1];
uintptr_t blk_client_data_phys[BLK_NUM_CLIENTS + 1];
size_t blk_client_data_size[BLK_NUM_CLIENTS + 1];

int driver_init(int uio_fd, void **maps, uintptr_t *maps_phys, int num_maps,
                int argc, char **argv) {
  LOG_UIO_BLOCK("Initialising...\n");

  /* Expects a storage_info map, request queue map, response queue map, virt
   * data map, and BLK_NUM_CLIENT data mappings.
   */
  if (num_maps != BLK_NUM_CLIENTS + 5) {
    LOG_UIO_BLOCK_ERR("Expecting %d maps, got %d\n", BLK_NUM_CLIENTS + 5,
                      num_maps);
    return -1;
  }

  if (argc != 1) {
    LOG_UIO_BLOCK_ERR("Expecting 1 driver argument, got %d\n", argc);
    return -1;
  }

  char *storage_path = argv[0];

  driver_blk_vmm_info_passing_t *vmm_info_passing =
      (driver_blk_vmm_info_passing_t *)maps[0];
  blk_storage_info = (blk_storage_info_t *)maps[1];
  blk_req_queue_t *req_queue = (blk_req_queue_t *)maps[2];
  blk_resp_queue_t *resp_queue = (blk_resp_queue_t *)maps[3];
  for (int i = 0; i < BLK_NUM_CLIENTS + 1; i++) {
    blk_client_data[i] = (uintptr_t)maps[4 + i];
    blk_client_data_phys[i] = vmm_info_passing->client_data_phys[i];
    blk_client_data_size[i] = vmm_info_passing->client_data_size[i];
  }

  LOG_UIO_BLOCK(
      "Storage info phys addr: 0x%lx, Request queue phys addr: 0x%lx, "
      "Response queue phys addr: 0x%lx\n",
      maps_phys[1], maps_phys[2], maps_phys[3]);

#ifdef DEBUG_UIO_BLOCK
  LOG_UIO_BLOCK("Virt data io addr, Virt data size:\n");
  printf("    0x%lx, 0x%lx\n", blk_client_data_phys[0],
         blk_client_data_size[0]);
  LOG_UIO_BLOCK("Client data io addr, Client data size:\n");
  for (int i = 1; i < BLK_NUM_CLIENTS + 1; i++) {
    printf("    client %i: 0x%lx, 0x%lx\n", i, blk_client_data_phys[i],
           blk_client_data_size[i]);
  }
#endif

  blk_queue_init(&h, req_queue, resp_queue, BLK_QUEUE_CAPACITY_DRIV);

  storage_fd = open(storage_path, O_RDWR);
  if (storage_fd < 0) {
    LOG_UIO_BLOCK_ERR("Failed to open storage driver: %s\n", strerror(errno));
    return -1;
  }
  LOG_UIO_BLOCK("Opened storage drive: %s\n", storage_path);

  struct stat storageStat;
  if (fstat(storage_fd, &storageStat) < 0) {
    LOG_UIO_BLOCK_ERR("Failed to get storage drive status: %s\n",
                      strerror(errno));
    return -1;
  }

  if (!S_ISBLK(storageStat.st_mode)) {
    LOG_UIO_BLOCK_ERR("Storage drive is of an unsupported type\n");
    return -1;
  }

  /* Set drive as read-write */
  int read_only_set = 0;
  if (ioctl(storage_fd, BLKROSET, &read_only_set) == -1) {
    LOG_UIO_BLOCK_ERR("Failed to set storage drive as read-write: %s\n",
                      strerror(errno));
    return -1;
  }

  /* Get read only status */
  int read_only;
  if (ioctl(storage_fd, BLKROGET, &read_only) == -1) {
    LOG_UIO_BLOCK_ERR("Failed to get storage drive read only status: %s\n",
                      strerror(errno));
    return -1;
  }
  blk_storage_info->read_only = (bool)read_only;

  /* Get logical sector size */
  int sector_size;
  if (ioctl(storage_fd, BLKSSZGET, &sector_size) == -1) {
    LOG_UIO_BLOCK_ERR("Failed to get storage drive sector size: %s\n",
                      strerror(errno));
    return -1;
  }
  blk_storage_info->sector_size = (uint16_t)sector_size;

  /* Get size */
  uint64_t size;
  if (ioctl(storage_fd, BLKGETSIZE64, &size) == -1) {
    LOG_UIO_BLOCK_ERR("Failed to get storage drive size: %s\n",
                      strerror(errno));
    return -1;
  }
  blk_storage_info->capacity = size / BLK_TRANSFER_SIZE;

  LOG_UIO_BLOCK("Raw block device: read_only=%d, sector_size=%d, "
                "capacity(sectors)=%ld\n",
                (int)blk_storage_info->read_only, blk_storage_info->sector_size,
                blk_storage_info->capacity);

  /* Optimal size */
  /* As far as I know linux does not let you query this from userspace, set as 0
   * to mean undefined */
  blk_storage_info->block_size = 0;

  /* Driver is ready to go, set ready in shared storage info page */
  __atomic_store_n(&blk_storage_info->ready, true, __ATOMIC_RELEASE);

  LOG_UIO_BLOCK("Driver initialized\n");

  return 0;
}

/* The virtualiser gives us an io address. We need to figure out which uio
 * mapping this corresponds to, so that we can fetch the corresponding mmaped
 * virt address.
 */
static inline uintptr_t io_to_virt(uintptr_t io_addr) {
  for (int i = 0; i < BLK_NUM_CLIENTS + 1; i++) {
    if (io_addr - blk_client_data_phys[i] < blk_client_data_size[i]) {
      LOG_UIO_BLOCK("io address 0x%lx corresponds to client %d\n", io_addr, i);
      return blk_client_data[i] + (io_addr - blk_client_data_phys[i]);
    }
  }
  /* Didn't find a match */
  assert(false);
  return 0;
}

void driver_notified(int *events_fds, int num_events) {
  int err = 0;
  _unused(err);
  blk_req_code_t req_code;
  uintptr_t req_io;
  uint32_t req_block_number;
  uint16_t req_count;
  uint32_t req_id;

  while (!blk_queue_empty_req(&h)) {
    err = blk_dequeue_req(&h, &req_code, &req_io, &req_block_number, &req_count,
                          &req_id);
    assert(!err);
    LOG_UIO_BLOCK("Received request: code=%d, io=0x%lx, block_number=%d, "
                  "count=%d, id=%d\n",
                  req_code, req_io, req_block_number, req_count, req_id);

    blk_resp_status_t status = BLK_RESP_OK;
    uint16_t success_count = 0;

    switch (req_code) {
    case BLK_REQ_READ: {
      int ret = lseek(storage_fd, (off_t)req_block_number * BLK_TRANSFER_SIZE,
                      SEEK_SET);
      if (ret < 0) {
        LOG_UIO_BLOCK_ERR("Failed to seek in storage: %s\n", strerror(errno));
        status = BLK_RESP_ERR_UNSPEC;
        success_count = 0;
        break;
      }
      LOG_UIO_BLOCK("Reading from storage\n");

      int bytes_read = read(storage_fd, (void *)io_to_virt(req_io),
                            req_count * BLK_TRANSFER_SIZE);
      LOG_UIO_BLOCK("Read from storage successfully: %d bytes\n", bytes_read);
      if (bytes_read < 0) {
        LOG_UIO_BLOCK_ERR("Failed to read from storage: %s\n", strerror(errno));
        status = BLK_RESP_ERR_UNSPEC;
        success_count = 0;
      } else {
        status = BLK_RESP_OK;
        success_count = bytes_read / BLK_TRANSFER_SIZE;
      }
      break;
    }
    case BLK_REQ_WRITE: {
      int ret = lseek(storage_fd, (off_t)req_block_number * BLK_TRANSFER_SIZE,
                      SEEK_SET);
      if (ret < 0) {
        LOG_UIO_BLOCK_ERR("Failed to seek in storage: %s\n", strerror(errno));
        status = BLK_RESP_ERR_UNSPEC;
        success_count = 0;
        break;
      }
      LOG_UIO_BLOCK("Writing to storage\n");
      int bytes_written = write(storage_fd, (void *)io_to_virt(req_io),
                                req_count * BLK_TRANSFER_SIZE);
      LOG_UIO_BLOCK("Wrote to storage successfully: %d bytes\n", bytes_written);
      if (bytes_written < 0) {
        LOG_UIO_BLOCK_ERR("Failed to write to storage: %s\n", strerror(errno));
        status = BLK_RESP_ERR_UNSPEC;
        success_count = 0;
      } else {
        status = BLK_RESP_OK;
        success_count = bytes_written / BLK_TRANSFER_SIZE;
      }
      break;
    }
    case BLK_REQ_FLUSH: {
      int ret = fsync(storage_fd);
      if (ret != 0) {
        LOG_UIO_BLOCK_ERR("Failed to flush storage: %s\n", strerror(errno));
        status = BLK_RESP_ERR_UNSPEC;
      } else {
        status = BLK_RESP_OK;
      }
      break;
    }
    case BLK_REQ_BARRIER: {
      int ret = fsync(storage_fd);
      if (ret != 0) {
        LOG_UIO_BLOCK_ERR("Failed to flush storage: %s\n", strerror(errno));
        status = BLK_RESP_ERR_UNSPEC;
      } else {
        status = BLK_RESP_OK;
      }
      break;
    }
    default:
      LOG_UIO_BLOCK_ERR("Unknown command code: %d\n", req_code);
      assert(false);
    }
    /* Response queue is never full if number of inflight requests <= response
     * queue capacity */
    err = blk_enqueue_resp(&h, status, success_count, req_id);
    assert(!err);
    LOG_UIO_BLOCK("Enqueued response: status=%d, success_count=%d, id=%d\n",
                  status, success_count, req_id);
  }

  uio_irq_enable();
  vmm_notify();
  LOG_UIO_BLOCK("Notified other side\n");
}
