#include <stddef.h>
#include <stdbool.h>
#include <microkit.h>

#ifndef MAX_PASSTHROUGH_IRQ
#define MAX_PASSTHROUGH_IRQ MICROKIT_MAX_CHANNELS
#endif

typedef void (*virq_ack_fn_t)(size_t vcpu_id, int irq, void *cookie);

bool virq_controller_init(size_t boot_vcpu_id);
bool virq_register(size_t vcpu_id, size_t virq_num, virq_ack_fn_t ack_fn, void *ack_data);
bool virq_inject(size_t vcpu_id, int irq);
bool virq_set_level(size_t vcpu_id, int irq, bool level);

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

