/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>

#include <sddf/blk/queue.h>
#include <blk_config.h>

#include <uio/libuio.h>
#include <uio/blk.h>

/* Uncomment this to enable debug logging */
// #define DEBUG_UIO_BLOCK

#if defined(DEBUG_UIO_BLOCK)
#define LOG_UIO_BLOCK(...) do{ printf("UIO_DRIVER(BLOCK)"); printf(": "); printf(__VA_ARGS__); }while(0)
#else
#define LOG_UIO_BLOCK(...) do{}while(0)
#endif

#define LOG_UIO_BLOCK_ERR(...) do{ printf("UIO_DRIVER(BLOCK)"); printf("|ERROR: "); printf(__VA_ARGS__); }while(0)

#define STORAGE_MAX_PATHNAME 64

int storage_fd;

blk_storage_info_t *blk_config;
blk_queue_handle_t h;
uintptr_t blk_data;

int driver_init(void **maps, uintptr_t *maps_phys, int num_maps, int argc, char **argv)
{
    LOG_UIO_BLOCK("Initialising...\n");

    if (num_maps != 4) {
        LOG_UIO_BLOCK_ERR("Expecting 4 maps, got %d\n", num_maps);
        return -1;
    }

    if (argc != 1) {
        LOG_UIO_BLOCK_ERR("Expecting 1 driver argument, got %d\n", argc);
        return -1;
    }

    char *storage_path = argv[0];

    blk_config = (blk_storage_info_t *)maps[0];
    blk_req_queue_t *req_queue = (blk_req_queue_t *)maps[1];
    blk_resp_queue_t *resp_queue = (blk_resp_queue_t *)maps[2];
    blk_data = (uintptr_t)maps[3];

    LOG_UIO_BLOCK("maps_phys[0]: 0x%lx, maps_phys[1]: 0x%lx, maps_phys[2]: 0x%lx, maps_phys[3]: 0x%lx\n", maps_phys[0],
                  maps_phys[1], maps_phys[2], maps_phys[3]);

    blk_queue_init(&h, req_queue, resp_queue, BLK_QUEUE_CAPACITY_DRIV);

    storage_fd = open(storage_path, O_RDWR);
    if (storage_fd < 0) {
        LOG_UIO_BLOCK_ERR("Failed to open storage drive: %s\n", strerror(errno));
        return -1;
    }
    LOG_UIO_BLOCK("Opened storage drive: %s\n", storage_path);

    struct stat storageStat;
    if (fstat(storage_fd, &storageStat) < 0) {
        LOG_UIO_BLOCK_ERR("Failed to get storage drive status: %s\n", strerror(errno));
        return -1;
    }

    if (!S_ISBLK(storageStat.st_mode)) {
        LOG_UIO_BLOCK_ERR("Storage drive is of an unsupported type\n");
        return -1;
    }

    /* Set drive as read-write */
    int read_only_set = 0;
    if (ioctl(storage_fd, BLKROSET, &read_only_set) == -1) {
        LOG_UIO_BLOCK_ERR("Failed to set storage drive as read-write: %s\n", strerror(errno));
        return -1;
    }

    /* Get read only status */
    int read_only;
    if (ioctl(storage_fd, BLKROGET, &read_only) == -1) {
        LOG_UIO_BLOCK_ERR("Failed to get storage drive read only status: %s\n", strerror(errno));
        return -1;
    }
    blk_config->read_only = (bool)read_only;

    /* Get logical sector size */
    int sector_size;
    if (ioctl(storage_fd, BLKSSZGET, &sector_size) == -1) {
        LOG_UIO_BLOCK_ERR("Failed to get storage drive sector size: %s\n", strerror(errno));
        return -1;
    }
    blk_config->sector_size = (uint16_t)sector_size;

    /* Get size */
    uint64_t size;
    if (ioctl(storage_fd, BLKGETSIZE64, &size) == -1) {
        LOG_UIO_BLOCK_ERR("Failed to get storage drive size: %s\n", strerror(errno));
        return -1;
    }
    blk_config->capacity = size / BLK_TRANSFER_SIZE;

    LOG_UIO_BLOCK("Raw block device: read_only=%d, sector_size=%d, size=%ld\n", (int)blk_config->read_only,
                  blk_config->sector_size, blk_config->capacity);

    /* Optimal size */
    /* As far as I know linux does not let you query this from userspace, set as 0 to mean undefined */
    blk_config->block_size = 0;

    /* Driver is ready to go, set ready in shared config page */
    __atomic_store_n(&blk_config->ready, true, __ATOMIC_RELEASE);

    LOG_UIO_BLOCK("Driver initialized\n");

    return 0;
}

void driver_notified()
{
    blk_req_code_t req_code;
    uintptr_t req_offset;
    uint32_t req_block_number;
    uint16_t req_count;
    uint32_t req_id;

    while (!blk_queue_empty_req(&h)) {
        blk_dequeue_req(&h, &req_code, &req_offset, &req_block_number, &req_count, &req_id);
        LOG_UIO_BLOCK("Received command: code=%d, offset=0x%lx, block_number=%d, count=%d, id=%d\n", req_code, req_offset,
                      req_block_number, req_count, req_id);

        blk_resp_status_t status = BLK_RESP_OK;
        uint16_t success_count = 0;

        if (blk_queue_full_resp(&h)) {
            LOG_UIO_BLOCK_ERR("Response ring is full, dropping response\n");
            continue;
        }

        switch (req_code) {
        case BLK_REQ_READ: {
            int ret = lseek(storage_fd, (off_t)req_block_number * BLK_TRANSFER_SIZE, SEEK_SET);
            if (ret < 0) {
                LOG_UIO_BLOCK_ERR("Failed to seek in storage: %s\n", strerror(errno));
                status = BLK_RESP_ERR_UNSPEC;
                success_count = 0;
                break;
            }
            LOG_UIO_BLOCK("Reading from storage at mmaped address: 0x%lx\n", req_offset + blk_data);
            int bytes_read = read(storage_fd, (void *)(req_offset + blk_data), req_count * BLK_TRANSFER_SIZE);
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
            int ret = lseek(storage_fd, (off_t)req_block_number * BLK_TRANSFER_SIZE, SEEK_SET);
            if (ret < 0) {
                LOG_UIO_BLOCK_ERR("Failed to seek in storage: %s\n", strerror(errno));
                status = BLK_RESP_ERR_UNSPEC;
                success_count = 0;
                break;
            }
            LOG_UIO_BLOCK("Writing to storage at mmaped address: 0x%lx\n", req_offset + blk_data);
            int bytes_written = write(storage_fd, (void *)(req_offset + blk_data), req_count * BLK_TRANSFER_SIZE);
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
            continue;
        }
        blk_enqueue_resp(&h, status, success_count, req_id);
        LOG_UIO_BLOCK("Enqueued response: status=%d, success_count=%d, id=%d\n", status, success_count, req_id);
    }

    uio_notify();
    LOG_UIO_BLOCK("Notified other side\n");
}
