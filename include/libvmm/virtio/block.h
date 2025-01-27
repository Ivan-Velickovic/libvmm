/*
 * Copyright 2019, DornerWorks
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* This header is BSD licensed so anyone can use the definitions to implement
 * compatible drivers/servers.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of IBM nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL IBM OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE. */

#pragma once

#include <stdint.h>
#include <libvmm/virtio/mmio.h>
#include <sddf/util/fsmalloc.h>
#include <sddf/util/ialloc.h>
#include <sddf/blk/queue.h>
#include <sddf/blk/storage_info.h>

/* Feature bits */
#define VIRTIO_BLK_F_SIZE_MAX       1   /* Indicates maximum segment size */
#define VIRTIO_BLK_F_SEG_MAX        2   /* Indicates maximum # of segments */
#define VIRTIO_BLK_F_GEOMETRY       4   /* Legacy geometry available  */
#define VIRTIO_BLK_F_RO             5   /* Disk is read-only */
#define VIRTIO_BLK_F_BLK_SIZE       6   /* Block size of disk is available*/
#define VIRTIO_BLK_F_FLUSH          9   /* Flush command supported */
#define VIRTIO_BLK_F_TOPOLOGY       10  /* Topology information is available */
#define VIRTIO_BLK_F_CONFIG_WCE     11  /* Writeback mode available in config */
#define VIRTIO_BLK_F_MQ             12  /* support more than one vq */
#define VIRTIO_BLK_F_DISCARD        13  /* DISCARD is supported */
#define VIRTIO_BLK_F_WRITE_ZEROES   14  /* WRITE ZEROES is supported */
#define VIRTIO_BLK_F_SECURE_ERASE   16 /* Secure Erase is supported */
#define VIRTIO_BLK_F_ZONED          17  /* Zoned block device */

/* Legacy feature bits */
#define VIRTIO_BLK_F_BARRIER        0   /* Does host support barriers? */
#define VIRTIO_BLK_F_SCSI           7   /* Supports scsi command passthru */

/* Old (deprecated) name for VIRTIO_BLK_F_WCE. */
#define VIRTIO_BLK_F_WCE VIRTIO_BLK_F_FLUSH

#define VIRTIO_BLK_ID_BYTES         20      /* ID string length */

struct virtio_blk_config {
    /* The capacity (in 512-byte sectors). */
    uint64_t capacity;
    /* The maximum segment size (if VIRTIO_BLK_F_SIZE_MAX) */
    uint32_t size_max;
    /* The maximum number of segments (if VIRTIO_BLK_F_SEG_MAX) */
    uint32_t seg_max;
    /* geometry the device (if VIRTIO_BLK_F_GEOMETRY) */
    struct virtio_blk_geometry {
        uint16_t cylinders;
        uint8_t heads;
        uint8_t sectors;
    } geometry;
    /* block size of device (if VIRTIO_BLK_F_BLK_SIZE) */
    uint32_t blk_size;
    /* the next 4 entries are guarded by VIRTIO_BLK_F_TOPOLOGY  */
    struct virtio_blk_topology {
        /* exponent for physical block per logical block. */
        uint8_t physical_block_exp;
        /* alignment offset in logical blocks. */
        uint8_t alignment_offset;
        /* minimum I/O size without performance penalty in logical blocks. */
        uint16_t min_io_size;
        /* optimal sustained I/O size in logical blocks. */
        uint32_t opt_io_size;
    } topology;
    /* writeback mode (if VIRTIO_BLK_F_CONFIG_WCE) */
    uint8_t writeback;
} __attribute__((packed));

/*
 * Command types
 *
 * Usage is a bit tricky as some bits are used as flags and some are not.
 *
 * Rules:
 *   VIRTIO_BLK_T_OUT may be combined with
 *   VIRTIO_BLK_T_BARRIER.  VIRTIO_BLK_T_FLUSH is a command of its own
 *   and may not be combined with any of the other flags.
 */

/* These two define direction. */
#define VIRTIO_BLK_T_IN             0
#define VIRTIO_BLK_T_OUT            1

/* Cache flush command */
#define VIRTIO_BLK_T_FLUSH          4

/* Get device ID command */
#define VIRTIO_BLK_T_GET_ID         8

/* Barrier before this op. */
#define VIRTIO_BLK_T_BARRIER    0x80000000

/* This is the first element of the read scatter-gather list. */
struct virtio_blk_outhdr {
    /* VIRTIO_BLK_T* */
    uint32_t type;
    /* io priority. */
    uint32_t ioprio;
    /* Sector (ie. 512 byte offset) */
    uint64_t sector;
} __attribute__((packed));

/* And this is the final byte of the write scatter-gather list. */
#define VIRTIO_BLK_S_OK             0
#define VIRTIO_BLK_S_IOERR          1
#define VIRTIO_BLK_S_UNSUPP         2

#define VIRTIO_BLK_SECTOR_SIZE 512

/* Device (backend) implementation */
#define SDDF_BLK_NUM_HANDLES 1
#define SDDF_BLK_DEFAULT_HANDLE 0
/* Maximum number of buffers in sddf data region */
#define SDDF_MAX_DATA_CELLS 8192
/* Maximum sddf queue capacity */
#define SDDF_MAX_QUEUE_CAPACITY 8192

#define VIRTIO_BLK_NUM_VIRTQ 1
#define VIRTIO_BLK_DEFAULT_VIRTQ 0

/* This struct exists to bookkeep request metadata when converting sddf requests
 * from a virtio request so that it can be later retrieved when converting a
 * virtio response from sddf response.
 */
typedef struct reqbk {
  // /* For writing response byte in virtio descriptor */
  // uint8_t *virtio_resp_byte;
  /* Descriptor head of the virtio request */
  uint16_t virtio_desc_head;
  /* For enqueuing sddf req/resp */
  uintptr_t sddf_data_cell_base;
  uint16_t sddf_count;
  uint32_t sddf_block_number;
  uintptr_t sddf_data;
  /* The size of data contained in virtio request */
  uint32_t virtio_body_size_bytes;
  /* Indicates this request is an unaligned write from virtIO. When not true,
   * this request is the "read" part of the read-modify-write. A subsequent
   * write request will be enqueued to complete the read-modify-write.
   */
  bool aligned;
} reqbk_t;

struct virtio_blk_device {
  struct virtio_device virtio_device;
  struct virtio_blk_config config;
  struct virtio_queue_handler vqs[VIRTIO_BLK_NUM_VIRTQ];
  /* Request bookkeep indexed by the request id */
  reqbk_t reqsbk[SDDF_MAX_QUEUE_CAPACITY];
  /* Data struct that handles allocation and freeing of fixed size data cells
   * in sDDF memory region */
  fsmalloc_t fsmalloc;
  bitarray_t fsmalloc_avail_bitarr;
  word_t fsmalloc_avail_bitarr_words[roundup_bits2words64(SDDF_MAX_DATA_CELLS)];
  /* Index allocator for sddf request ids */
  ialloc_t ialloc;
  uint32_t ialloc_idxlist[SDDF_MAX_QUEUE_CAPACITY];
  /* Sddf structures */
  blk_storage_info_t *storage_info;
  blk_queue_handle_t queue_h;
  uint32_t queue_capacity;
  uintptr_t data_region;
  /* Channel to notify microkit component serving this client */
  int server_ch;
};

bool virtio_mmio_blk_init(struct virtio_blk_device *blk_dev,
                          uintptr_t region_base, uintptr_t region_size,
                          size_t virq, uintptr_t data_region,
                          size_t data_region_size,
                          blk_storage_info_t *storage_info,
                          blk_queue_handle_t *queue_h, uint32_t queue_capacity,
                          int server_ch);

bool virtio_blk_handle_resp(struct virtio_blk_device *blk_dev);
