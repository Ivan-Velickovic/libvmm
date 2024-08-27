/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stddef.h>
#include <stdbool.h>
#include <microkit.h>

#ifndef MAX_PASSTHROUGH_IRQ
#define MAX_PASSTHROUGH_IRQ MICROKIT_MAX_CHANNELS
#endif

typedef void (*virq_ack_fn_t)(size_t vcpu_id, int irq, void *cookie);

/*
 * Initialise the architecture-depedent virtual interrupt controller.
 * On ARM, this is the virtual Generic Interrupt Controller (vGIC).
 */
bool virq_controller_init();
bool virq_register(size_t vcpu_id, size_t virq_num, virq_ack_fn_t ack_fn, void *ack_data);

/*
 * Inject an IRQ into the boot virtual CPU.
 * Note that this API requires that the IRQ has been registered (with virq_register).
 */
bool virq_inject(int irq);

/*
 * The same functionality as virq_inject, but will inject the virtual IRQ into a specific
 * virtual CPU.
 */
bool virq_inject_vcpu(size_t vcpu_id, int irq);

/*
 * These two APIs are convenient for when you want to directly passthrough an IRQ from
 * the hardware to the guest as the same vIRQ. This is useful when the guest has direct
 * passthrough access to a particular device on the hardware.
 * After registering the passthrough IRQ, call `virq_handle_passthrough` when
 * the IRQ has come through from seL4.
 *
 * @ivanv: currently this API assumes a Microkit environment. This should be changed
 * if/when we make libvmm agnostic to the seL4 environment it is running in.
 */
bool virq_register_passthrough(size_t vcpu_id, size_t irq, microkit_channel irq_ch);
bool virq_handle_passthrough(microkit_channel irq_ch);
