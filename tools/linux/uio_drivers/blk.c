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
#include <sddf/blk/shared_queue.h>

#include <uio_drivers/libuio.h>
#include <uio_drivers/blk.h>

/* Uncomment this to enable debug logging */
// #define DEBUG_UIO_BLOCK

#if defined(DEBUG_UIO_BLOCK)
#define LOG_UIO_BLOCK(...) do{ printf("UIO_BLK_DRIVER_%d", id); printf(": "); printf(__VA_ARGS__); }while(0)
#else
#define LOG_UIO_BLOCK(...) do{}while(0)
#endif

#define LOG_UIO_BLOCK_ERR(...) do{ printf("UIO_BLK_DRIVER_%d", id); printf("|ERROR: "); printf(__VA_ARGS__); }while(0)

#define STORAGE_MAX_PATHNAME 64

int id;
int storage_fd;

blk_storage_info_t *blk_config;
blk_queue_handle_t h;
uintptr_t blk_data;
uintptr_t blk_data_phys;

uintptr_t data_phys_to_virt(uintptr_t phys_addr)
{
    return phys_addr - blk_data_phys + blk_data;
}

int driver_init(int driver_id, void **maps, uintptr_t *maps_phys, int num_maps, int argc, char **argv)
{   
    id = driver_id;
    
    if (num_maps != 4) {
        LOG_UIO_BLOCK_ERR("Expecting 4 maps, got %d\n", num_maps);
        return -1;
    }
    
    if (argc != 1) {
        LOG_UIO_BLOCK_ERR("Expecting 1 driver argument, got %d\n", argc);
        return -1;
    }

    char *storage_base_path = argv[0];
    
    blk_config = (blk_storage_info_t *)maps[0];
    blk_req_queue_t *req_queue = (blk_req_queue_t *)maps[1];
    blk_resp_queue_t *resp_queue = (blk_resp_queue_t *)maps[2];
    blk_data = (uintptr_t)maps[3];
    blk_data_phys = maps_phys[3];

    LOG_UIO_BLOCK("blk_data_phys: 0x%lx\n", blk_data_phys);

    blk_queue_init(&h, req_queue, resp_queue, false, BLK_REQ_QUEUE_SIZE, BLK_RESP_QUEUE_SIZE);

    // @TODO, @ericc: Need to figure out how to determine config values.
    // These numbers will be evaluated depending on the policy of the MUX,
    // and the actual device firmware itself.
    // For now, I've hardcoded them.
    blk_config->size = 1000;
    blk_config->blocksize = 1024;
    blk_config->read_only = false;

    char storage_path[STORAGE_MAX_PATHNAME];
    int len = snprintf(storage_path, STORAGE_MAX_PATHNAME, "%s/storage_%d", storage_base_path, driver_id);
    if (len < 0 || len >= STORAGE_MAX_PATHNAME) {
        LOG_UIO_BLOCK_ERR("Failed to generate storage path\n");
        return -1;
    }

    if ((storage_fd = open(storage_path, O_RDWR)) < 0) {
        LOG_UIO_BLOCK_ERR("Failed to open storage file: %s\n", strerror(errno));
        return -1;
    }
    
    // @ericc: maybe need to flush all writes before this point of setting ready = true
    blk_config->ready = true;
    
    LOG_UIO_BLOCK("Driver initialized\n");
    
    return 0;
}

void driver_notified()
{
    uint16_t blocksize = blk_config->blocksize;
    blk_request_code_t req_code;
    uintptr_t req_addr;
    uint32_t req_block_number;
    uint16_t req_count;
    uint32_t req_id;

    while (!blk_req_queue_empty(&h)) {
        blk_dequeue_req(&h, &req_code, &req_addr, &req_block_number, &req_count, &req_id);
        LOG_UIO_BLOCK("Received command: code=%d, addr=%p, block_number=%d, count=%d, id=%d\n", req_code, (void *)req_addr, req_block_number, req_count, req_id);

        blk_response_status_t status = SUCCESS;
        uint16_t success_count = 0;

        // @ericc: These should be the same across all requests - we return what the request gives us
        uintptr_t addr = req_addr;
        uint16_t count = req_count;
        uint32_t id = req_id;
        
        //@ericc: what happens if this response is dropped? should we have a timeout in client?
        if (blk_resp_queue_full(&h)) {
            LOG_UIO_BLOCK_ERR("Response ring is full, dropping response\n");
            continue;
        }

        // TODO: @ericc: workout what error status are appropriate, sDDF block only contains SEEK_ERROR right now
        switch(req_code) {
            case READ_BLOCKS:
                lseek(storage_fd, req_block_number * blocksize, SEEK_SET);
                int bytes_read = read(storage_fd, (void *)data_phys_to_virt(req_addr), req_count * blocksize);
                if (bytes_read < 0) {
                    LOG_UIO_BLOCK_ERR("Failed to read from storage: %s\n", strerror(errno));
                    status = SEEK_ERROR;                    
                    success_count = 0;
                } else {
                    status = SUCCESS;
                    success_count = bytes_read / blocksize;
                }
                break;
            case WRITE_BLOCKS:
                lseek(storage_fd, req_block_number * blocksize, SEEK_SET);
                int bytes_written = write(storage_fd, (void *)data_phys_to_virt(req_addr), req_count * blocksize);
                if (bytes_written < 0) {
                    LOG_UIO_BLOCK_ERR("Failed to write to storage: %s\n", strerror(errno));
                    status = SEEK_ERROR;
                    success_count = 0;
                } else {
                    status = SUCCESS;
                    success_count = bytes_written / blocksize;
                }
                break;
            case FLUSH:
                fsync(storage_fd);
                status = SUCCESS;
                break;
            case BARRIER:
                fsync(storage_fd);
                status = SUCCESS;
                break;
            default:
                LOG_UIO_BLOCK_ERR("Unknown command code: %d\n", req_code);
                continue;
        }
        blk_enqueue_resp(&h, status, addr, count, success_count, id);    
    }

    uio_notify();
}
