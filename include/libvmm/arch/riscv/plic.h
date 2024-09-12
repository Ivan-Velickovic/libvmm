#include <microkit.h>
#include <stdbool.h>
#include <stddef.h>
#include <libvmm/virq.h>

// TODO: move out of here
#define PLIC_ADDR 0xc000000
#define PLIC_SIZE 0x4000000

bool plic_handle_fault(size_t vcpu_id, size_t offset, seL4_Word fsr, seL4_UserContext *regs);

bool plic_inject_timer_irq(size_t vcpu_id);

bool plic_inject_irq(size_t vcpu_id, int irq);
bool plic_register_irq(size_t vcpu_id, size_t irq, virq_ack_fn_t ack_fn, void *ack_data);
