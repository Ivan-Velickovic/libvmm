#include <microkit.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/riscv/plic.h>
#include <libvmm/arch/riscv/fault.h>

// TODO: defined both here and in fault.c...
#define SIP_TIMER (1 << 5)

bool plic_inject_timer_irq(size_t vcpu_id) {
    LOG_VMM("injecting timer irq\n");
    seL4_RISCV_VCPU_ReadRegs_t res = seL4_RISCV_VCPU_ReadRegs(BASE_VCPU_CAP + vcpu_id, seL4_VCPUReg_SIP);
    assert(!res.error);
    seL4_Word sip = res.value;
    // res = seL4_RISCV_VCPU_ReadRegs(BASE_VCPU_CAP + vcpu_id, seL4_VCPUReg_SIE);
    // assert(!res.error);
    // TODO: we don't actually do anything with SIE, we should probably
    // be checking that the timer interrupt is even enabled, right?
    // seL4_Word sie = res.value;
    sip |= SIP_TIMER;
    int err = seL4_RISCV_VCPU_WriteRegs(BASE_VCPU_CAP + vcpu_id, seL4_VCPUReg_SIP, sip);
    assert(!err);

    return true;
}

bool plic_handle_fault(size_t vcpu_id, size_t offset, seL4_Word fsr, seL4_UserContext *regs) {
    fault_decode_instruction(vcpu_id, regs, seL4_GetMR(seL4_UserException_FaultIP));
    return false;
}
