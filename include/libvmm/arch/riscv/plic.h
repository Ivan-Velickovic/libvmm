#include <microkit.h>
#include <stdbool.h>
#include <stddef.h>

bool plic_handle_fault(size_t vcpu_id, size_t offset, seL4_Word fsr, seL4_UserContext *regs);

bool plic_inject_timer_irq(size_t vcpu_id);
