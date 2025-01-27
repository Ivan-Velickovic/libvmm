/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <microkit.h>
#include <stdint.h>

/* This sets up the resources needed by a userspace driver. Includes registering
 * the uio virq and vmm notify region. The uio driver will write into the notify
 * region in order to transfer execution to the VMM for it to then notify other
 * microkit components.
 */
bool uio_register_driver(int irq, microkit_channel *ch,
                         uintptr_t notify_region_base,
                         size_t notify_region_size);
