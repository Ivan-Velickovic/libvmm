/*
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <microkit.h>

/* Fault-handling functions */
bool fault_handle(size_t vcpu_id, microkit_msginfo msginfo);

void fault_decode_instruction(size_t vcpu_id, seL4_UserContext *regs, seL4_Word ip);
