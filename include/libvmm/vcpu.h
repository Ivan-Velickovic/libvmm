/*
 * Copyright 2023, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Checks whether the given vCPU is on.
 *
 * Returns true if the vCPU is on, otherwise false.
 * Returns false if an invalid vCPU ID is given.
 */
bool vcpu_is_on(size_t vcpu_id);

/*
 * Change the state tracking for whether a given vCPU is on.
 * Does nothing if an invalid vCPU ID is given.
 */
void vcpu_set_on(size_t vcpu_id, bool on);

void vcpu_reset(size_t vcpu_id);
void vcpu_print_regs(size_t vcpu_id);
