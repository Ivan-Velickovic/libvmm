#include <microkit.h>
#include <libvmm/virq.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/riscv/plic.h>
#include <libvmm/arch/riscv/fault.h>

// #define DEBUG_PLIC

#if defined(DEBUG_PLIC)
#define LOG_PLIC(...) do{ LOG_VMM("PLIC: "); printf(__VA_ARGS__); }while(0)
#else
#define LOG_PLIC(...) do{}while(0)
#endif

/*
 * Note that this register map is not intended to match
 * the layout of the device itself as we are virtualising access
 * to the PLIC register by register.
 * For example, the priority threshold data is stored contigiously
 * here, while the PLIC device does not.
 */

// TODO: have no fucking idea where this number comes from
#define PLIC_NUM_CONTEXTS 15872

struct plic_regs {
    uint32_t priority[1024];
    uint32_t pending_bits[32];
    uint32_t enable_bits[PLIC_NUM_CONTEXTS][32];
    uint32_t priority_threshold[PLIC_NUM_CONTEXTS];
};

struct plic_regs plic_regs;

// TODO: defined both here and in fault.c...
#define SIP_TIMER (1 << 5)

bool plic_inject_timer_irq(size_t vcpu_id) {
    // LOG_VMM("injecting timer irq\n");
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

#define SIP_EXTERNAL (1 << 9)

// TODO: this is for context 0
#define PLIC_IRQ_ENABLE_START 0x2000
#define PLIC_IRQ_ENABLE_END 0x1F1FFC

// TODO: need to check whether the ENDs are correct, I think they might be off by one
#define PLIC_IRQ_PRIORITY_START 0x0
#define PLIC_IRQ_PRIORITY_END 0xFFC

#define PLIC_PRIOTIY_THRESHOLD_CONTEXT_1_START  0x201000
#define PLIC_PRIOTIY_THRESHOLD_CONTEXT_1_END    0x201004
#define PLIC_CLAIM_COMPLETE_CONTEXT_1_START     0x201004
#define PLIC_CLAIM_COMPLETE_CONTEXT_1_END       0x201008

uint32_t plic_pending_irq = 0;

static bool plic_handle_fault_read(size_t vcpu_id, size_t offset, seL4_UserContext *regs, struct fault_instruction *instruction) {
    LOG_PLIC("handling read at offset: 0x%lx\n", offset);

    uint32_t data;
    switch (offset) {
    case PLIC_IRQ_ENABLE_START...PLIC_IRQ_ENABLE_END: {
        size_t context = (offset - PLIC_IRQ_ENABLE_START) / 128;
        /* Now that we have the context, we want to find what the IRQ source is so
         * we can figure out the second-level index. */
        size_t enable_group = (offset - PLIC_IRQ_ENABLE_START) % 128;
        LOG_PLIC("reading enable bits for context %d, IRQ source #%d to #%d\n", context, enable_group * 32, ((enable_group + 1) * 32 - 1));
        data = plic_regs.enable_bits[context][enable_group];
        break;
    }
    case PLIC_CLAIM_COMPLETE_CONTEXT_1_START: {
        LOG_PLIC("read complete claim for pending IRQ %d\n", plic_pending_irq);
        data = plic_pending_irq;
        plic_pending_irq = 0;
        seL4_RISCV_VCPU_ReadRegs_t sip = seL4_RISCV_VCPU_ReadRegs(BASE_VCPU_CAP + vcpu_id, seL4_VCPUReg_SIP);
        assert(!sip.error);
        sip.value &= ~SIP_EXTERNAL;
        int err = seL4_RISCV_VCPU_WriteRegs(BASE_VCPU_CAP + vcpu_id, seL4_VCPUReg_SIP, sip.value);
        assert(!err);
        break;
    }
    default:
        LOG_PLIC("invalid offset 0x%lx\n", offset);
        return false;
    }
    assert(instruction->width == 2 || instruction->width == 4);

    // TODO: we can do this better probably
    seL4_Word reg;
    if (instruction->width == 2) {
        reg = fault_get_reg_compressed(regs, instruction->rd);
    } else {
        reg = fault_get_reg(regs, instruction->rd);
    }

    reg &= 0xffffffff00000000;
    reg |= data;
    if (instruction->width == 2) {
        fault_set_reg_compressed(regs, instruction->rd, reg);
    } else {
        fault_set_reg(regs, instruction->rd, reg);
    }

    return true;
}

static bool plic_handle_fault_write(size_t vcpu_id, size_t offset, seL4_UserContext *regs, struct fault_instruction *instruction) {
    LOG_PLIC("handling write at offset: 0x%lx\n", offset);

    // TODO: need to make sure offset is 4-byte aligned?

    uint32_t data;
    if (instruction->width == 2) {
        data = fault_get_reg_compressed(regs, instruction->rs2);
    } else {
        data = fault_get_reg(regs, instruction->rs2);
    }

    assert(instruction->width == 2 || instruction->width == 4);

    switch (offset) {
    case PLIC_IRQ_ENABLE_START...PLIC_IRQ_ENABLE_END: {
        size_t context = (offset - PLIC_IRQ_ENABLE_START) / 128;
        /* Now that we have the context, we want to find what the IRQ source is so
         * we can figure out the second-level index. */
        size_t enable_group = (offset - PLIC_IRQ_ENABLE_START) % 128;
        LOG_PLIC("writing enable bits for context %d, IRQ source #%d to #%d: data 0x%lx\n", context, enable_group * 32, ((enable_group + 1) * 32 - 1), data);
        plic_regs.enable_bits[context][enable_group] = data;
        break;
    }
    case PLIC_IRQ_PRIORITY_START...PLIC_IRQ_PRIORITY_END: {
        size_t irq_index = (offset - PLIC_IRQ_PRIORITY_START) / 0x4;
        if (irq_index == 0) {
            LOG_VMM_ERR("writing to invalid IRQ priority index\n");
            return false;
        }

        LOG_PLIC("write priority for IRQ source %d: %d\n", irq_index, data);
        plic_regs.priority[irq_index] = data;
        break;
    }
    case PLIC_PRIOTIY_THRESHOLD_CONTEXT_1_START: {
        LOG_PLIC("write priority threshold for context %d: %d\n", 1, data);
        plic_regs.priority_threshold[1] = data;
        break;
    }
    case PLIC_CLAIM_COMPLETE_CONTEXT_1_START: {
        LOG_PLIC("write complete claim for pending IRQ %d\n", plic_pending_irq);
        /* TODO: we should be checking here, and probably in a lot of other places, that the
         * IRQ attempting to be claimed is actually enabled. */
        /* TODO: double check but when we get a claim we should be clearing the pending bit */
        // size_t irq_pending_group = plic_pending_irq / 32;
        plic_pending_irq = 0;
        // plic_regs.pending_bits[irq_pending_group] = 0;
        microkit_irq_ack(1);
        break;
    }
    default:
        LOG_PLIC("invalid offset 0x%lx\n", offset);
        return false;
    }

    return true;
}

#define PLIC_MAX_REGISTERED_IRQS 10

struct virq {
    int irq;
    virq_ack_fn_t ack_fn;
    void *ack_data;
};

struct virq plic_registered_irqs[PLIC_MAX_REGISTERED_IRQS];
size_t plic_register_count = 0;

bool plic_register_irq(size_t vcpu_id, size_t irq, virq_ack_fn_t ack_fn, void *ack_data) {
    // todo: totally redo this bit with error-checking involved and I need to probably
    // distinguish between cpu local vs external irqs? what if we're emulating the architectural
    // timer?
    plic_registered_irqs[plic_register_count] = (struct virq) {
        .irq = irq,
        .ack_fn = ack_fn,
        .ack_data = ack_data,
    };
    plic_register_count += 1;
    assert(plic_register_count <= 10);

    return true;
}

bool plic_inject_irq(size_t vcpu_id, int irq) {
    // size_t context = irq / 128;
    size_t enable_group = irq / 128;
    if ((plic_regs.enable_bits[1][enable_group] & (1 << irq)) == 0) {
        return true;
    }

    seL4_RISCV_VCPU_ReadRegs_t res = seL4_RISCV_VCPU_ReadRegs(BASE_VCPU_CAP + vcpu_id, seL4_VCPUReg_SIP);
    assert(!res.error);
    seL4_Word sip = res.value;

    size_t irq_pending_group = irq / 32;
    size_t irq_bit = irq % 32;

    // LOG_PLIC("injecting for IRQ %d (group: %d, bit: %d)\n", irq, irq_pending_group, irq_bit);

    // TODO: should we check if it's already pending?

    // assert(plic_pending_irq == 0);
    plic_pending_irq = irq;

    plic_regs.pending_bits[irq_pending_group] |= 1 << irq_bit;

    sip |= SIP_EXTERNAL;

    int err = seL4_RISCV_VCPU_WriteRegs(BASE_VCPU_CAP + vcpu_id, seL4_VCPUReg_SIP, sip);
    assert(!err);

    return true;
}

bool plic_handle_fault(size_t vcpu_id, size_t offset, seL4_Word fsr, seL4_UserContext *regs) {
    struct fault_instruction instruction = fault_decode_instruction(vcpu_id, regs, regs->pc);
    assert(instruction.op_code != 0);
    /* from decode instruction we need: opcode, rs2, and rd */

    /* TODO: why not just check the fsr? */
    bool success;
    switch (instruction.op_code) {
    case OP_CODE_LOAD:
        success = plic_handle_fault_read(vcpu_id, offset, regs, &instruction);
        break;
    case OP_CODE_STORE:
        success = plic_handle_fault_write(vcpu_id, offset, regs, &instruction);
        break;
    default:
        LOG_VMM_ERR("invalid op code when handling PLIC fault\n");
        return false;
    }

    regs->pc += instruction.width;
    seL4_TCB_WriteRegisters(BASE_VM_TCB_CAP + vcpu_id, false, 0, sizeof(seL4_UserContext) / sizeof(seL4_Word), regs);

    return success;
}
