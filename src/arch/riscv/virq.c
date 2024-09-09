#include <libvmm/virq.h>
#include <libvmm/util/util.h>

#define PLIC_ADDR 0xc000000

bool virq_controller_init(size_t boot_vcpu_id) {
    // @riscv
    return true;
}

bool virq_inject(size_t vcpu_id, int irq) {
    // @riscv
    return false;
}

bool virq_register(size_t vcpu_id, size_t virq_num, virq_ack_fn_t ack_fn, void *ack_data) {
    // @riscv
    return false;
}
