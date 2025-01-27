/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

/* Notify the VMM by writing into a registered
 * memory region thereby invoking a hyp trap.
 */
void vmm_notify();

/* Writing 1 to the UIO device re-enables the
 * interrupt.
 */
void uio_irq_enable();

/* Drivers can add their own events to the uio event loop */
void bind_fd_to_event_loop(int fd);
