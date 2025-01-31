/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

#include <ethernet_config.h>

typedef struct {
    /* Any info that the VMM wants to give us go in here */

    /* We need these as the network virtualisers give us physical addresses
       in the data region, so we need the base to get the offsets to actually
       read the data. */
    uint64_t rx_paddr;
    uint64_t tx_paddrs[NUM_NETWORK_CLIENTS];
} vmm_net_info_t;

/* These are where the UIO shared memory regions and IRQs live */
#define UIO_PATH_SDDF_NET_CONTROL_AND_DATA_QUEUES "/dev/uio0"

/* This is how the VMM notify us of notifications from virt TX and RX.
   Once IRQ is enabled by writing to a UIO FD from these, a read on the same
   FD will block until the VMM does a virq_inject on the associated IRQ number
   in device tree. */
#define UIO_PATH_SDDF_NET_INCOMING_TX_IRQ "/dev/uio1"
#define UIO_PATH_SDDF_NET_INCOMING_RX_IRQ "/dev/uio2"

/* Data passing between the VMM and guest. Currently it is used for passing the
   physical addresses of the data regions */
#define UIO_PATH_SDDF_NET_SHARED_DATA "/dev/uio3"

/* This is how the guest can signal back to VMM, the physical address of these
   UIO regions are unmapped, so a fault is generated */
#define UIO_PATH_SDDF_NET_TX_FAULT_TO_VMM "/dev/uio4"
#define UIO_PATH_SDDF_NET_RX_FAULT_TO_VMM "/dev/uio5"
#define GUEST_TO_VMM_TX_FAULT_ADDR 0x60000000
#define GUEST_TO_VMM_RX_FAULT_ADDR 0x70000000
