/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MAX_BLK_CLIENTS 64

typedef struct driver_blk_vmm_info_passing {
  uintptr_t client_data_phys[MAX_BLK_CLIENTS];
  size_t client_data_size[MAX_BLK_CLIENTS];
} driver_blk_vmm_info_passing_t;

int driver_init(int uio_fd, void **maps, uintptr_t *maps_phys, int num_maps,
                int argc, char **argv);
void driver_notified(int *events_fd, int num_events);
