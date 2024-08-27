/*
 * Copyright 2017, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * This component controls and maintains the GIC for the VM.
 * IRQs must be registered at init time with vm_virq_new(...)
 * This function creates and registers an IRQ data structure which will be used for IRQ maintenance
 * b) ENABLING: When the VM enables the IRQ, it checks the pending flag for the VM.
 *   - If the IRQ is not pending, we either
 *        1) have not received an IRQ so it is still enabled in seL4
 *        2) have received an IRQ, but ignored it because the VM had disabled it.
 *     In either case, we simply ACK the IRQ with seL4. In case 1), the IRQ will come straight through,
       in case 2), we have ACKed an IRQ that was not yet pending anyway.
 *   - If the IRQ is already pending, we can assume that the VM has yet to ACK the IRQ and take no further
 *     action.
 *   Transitions: b->c
 * c) PIRQ: When an IRQ is received from seL4, seL4 disables the IRQ and sends an async message. When the VMM
 *    receives the message.
 *   - If the IRQ is enabled, we set the pending flag in the VM and inject the appropriate IRQ
 *     leading to state d)
 *   - If the IRQ is disabled, the VMM takes no further action, leading to state b)
 *   Transitions: (enabled)? c->d :  c->b
 * d) When the VM acknowledges the IRQ, an exception is raised and delivered to the VMM. When the VMM
 *    receives the exception, it clears the pending flag and acks the IRQ with seL4, leading back to state c)
 *    Transition: d->c
 * g) When/if the VM disables the IRQ, we may still have an IRQ resident in the GIC. We allow
 *    this IRQ to be delivered to the VM, but subsequent IRQs will not be delivered as seen by state c)
 *    Transitions g->c
 *
 *   NOTE: There is a big assumption that the VM will not manually manipulate our pending flags and
 *         destroy our state. The affects of this will be an IRQ that is never acknowledged and hence,
 *         will never occur again.
 */

#include <stdint.h>

#include <libvmm/arch/aarch64/fault.h>
#include <libvmm/arch/aarch64/vgic/vgic.h>
#include <libvmm/arch/aarch64/vgic/virq.h>
#include <libvmm/arch/aarch64/vgic/vgic_v3.h>
#include <libvmm/arch/aarch64/vgic/vdist.h>

vgic_t vgic;

static bool vgic_handle_fault_redist_read(size_t vcpu_id, vgic_t *vgic, uint64_t offset, uint64_t fsr,
                                          seL4_UserContext *regs)
{
    struct gic_dist_map *gic_dist = vgic_get_dist(vgic->registers);
    struct gic_redist_map *gic_redist = vgic_get_redist(vgic->registers);
    uint32_t reg = 0;
    uintptr_t base_reg;
    uint32_t *reg_ptr;
    switch (offset) {
    case RANGE32(GICR_CTLR, GICR_CTLR):
        reg = gic_redist->ctlr;
        break;
    case RANGE32(GICR_IIDR, GICR_IIDR):
        reg = gic_redist->iidr;
        break;
    case RANGE32(GICR_TYPER, GICR_TYPER):
        reg = gic_redist->typer;
        break;
    case RANGE32(GICR_WAKER, GICR_WAKER):
        reg = gic_redist->waker;
        break;
    case RANGE32(0xFFD0, 0xFFFC):
        base_reg = (uintptr_t) & (gic_redist->pidr4);
        reg_ptr = (uint32_t *)(base_reg + (offset - 0xFFD0));
        reg = *reg_ptr;
        break;
    case RANGE32(GICR_IGROUPR0, GICR_IGROUPR0):
        base_reg = (uintptr_t) & (gic_dist->irq_group0[vcpu_id]);
        reg_ptr = (uint32_t *)(base_reg + (offset - GICR_IGROUPR0));
        reg = *reg_ptr;
        break;
    case RANGE32(GICR_ICFGR1, GICR_ICFGR1):
        base_reg = (uintptr_t) & (gic_dist->config[1]);
        reg_ptr = (uint32_t *)(base_reg + (offset - GICR_ICFGR1));
        reg = *reg_ptr;
        break;
    default:
        LOG_VMM_ERR("Unknown register offset 0x%x\n", offset);
        // @ivanv: used to be ignore_fault, double check this is right
        bool success = fault_advance_vcpu(vcpu_id, regs);
        // @ivanv: todo error handling
        assert(success);
    }

    uintptr_t fault_addr = GIC_REDIST_PADDR + offset;
    uint32_t mask = fault_get_data_mask(fault_addr, fsr);
    fault_emulate_write(regs, fault_addr, fsr, reg & mask);
    // @ivanv: todo error handling

    return true;
}

static bool vgic_handle_fault_redist_write(size_t vcpu_id, vgic_t *vgic, uint64_t offset, uint64_t fsr,
                                           seL4_UserContext *regs)
{
    // @ivanv: why is this not reading from the redist?
    uintptr_t fault_addr = GIC_REDIST_PADDR + offset;
    struct gic_dist_map *gic_dist = vgic_get_dist(vgic->registers);
    uint32_t mask = fault_get_data_mask(GIC_REDIST_PADDR + offset, fsr);
    uint32_t data;
    switch (offset) {
    case RANGE32(GICR_WAKER, GICR_WAKER):
        /* Writes are ignored */
        break;
    case RANGE32(GICR_IGROUPR0, GICR_IGROUPR0):
        emulate_reg_write_access(regs, fault_addr, fsr, &gic_dist->irq_group0[vcpu_id]);
        break;
    case RANGE32(GICR_ISENABLER0, GICR_ISENABLER0):
        data = fault_get_data(regs, fsr);
        /* Mask the data to write */
        data &= mask;
        while (data) {
            int irq;
            irq = CTZ(data);
            data &= ~(1U << irq);
            vgic_dist_enable_irq(vgic, vcpu_id, irq);
        }
        break;
    case RANGE32(GICR_ICENABLER0, GICR_ICENABLER0):
        data = fault_get_data(regs, fsr);
        /* Mask the data to write */
        data &= mask;
        while (data) {
            int irq;
            irq = CTZ(data);
            data &= ~(1U << irq);
            set_enable(gic_dist, irq, false, vcpu_id);
        }
        break;
    case RANGE32(GICR_ICACTIVER0, GICR_ICACTIVER0):
        // @ivanv: understand, this is a comment left over from kent
        // TODO fix this
        emulate_reg_write_access(regs, fault_addr, fsr, &gic_dist->active0[vcpu_id]);
        break;
    case RANGE32(GICR_IPRIORITYR0, GICR_IPRIORITYRN):
        break;
    default:
        LOG_VMM_ERR("Unknown register offset 0x%x, value: 0x%x\n", offset, fault_get_data(regs, fsr));
    }

    return true;
}

bool vgic_handle_fault_redist(size_t vcpu_id, size_t offset, size_t fsr, seL4_UserContext *regs, void *data)
{
    if (fault_is_read(fsr)) {
        return vgic_handle_fault_redist_read(vcpu_id, &vgic, offset, fsr, regs);
    } else {
        return vgic_handle_fault_redist_write(vcpu_id, &vgic, offset, fsr, regs);
    }
}

static void vgic_dist_reset(struct gic_dist_map *dist)
{
    // @ivanv: come back to, right now it's a global so we don't need to init the memory to zero
    // memset(gic_dist, 0, sizeof(*gic_dist));

    dist->typer            = 0x7B04B0; /* RO */
    dist->iidr             = 0x1043B ; /* RO */

    dist->enable_set[0]    = 0x0000ffff; /* 16bit RO */
    dist->enable_clr[0]    = 0x0000ffff; /* 16bit RO */

    dist->config[0]        = 0xaaaaaaaa; /* RO */

    dist->pidrn[0]         = 0x44;     /* RO */
    dist->pidrn[4]         = 0x92;     /* RO */
    dist->pidrn[5]         = 0xB4;     /* RO */
    dist->pidrn[6]         = 0x3B;     /* RO */

    dist->cidrn[0]         = 0x0D;     /* RO */
    dist->cidrn[1]         = 0xF0;     /* RO */
    dist->cidrn[2]         = 0x05;     /* RO */
    dist->cidrn[3]         = 0xB1;     /* RO */
}

static void vgic_redist_reset(struct gic_redist_map *redist)
{
    // @ivanv: come back to, right now it's a global so we don't need to init the memory to zero
    // memset(redist, 0, sizeof(*redist));
    redist->typer           = 0x11;      /* RO */
    redist->iidr            = 0x1143B;  /* RO */

    redist->pidr0           = 0x93;     /* RO */
    redist->pidr1           = 0xB4;     /* RO */
    redist->pidr2           = 0x3B;     /* RO */
    redist->pidr4           = 0x44;     /* RO */

    redist->cidr0           = 0x0D;     /* RO */
    redist->cidr1           = 0xF0;     /* RO */
    redist->cidr2           = 0x05;     /* RO */
    redist->cidr3           = 0xB1;     /* RO */
}

// @ivanv: come back to
struct gic_dist_map dist;
struct gic_redist_map redist;
vgic_reg_t vgic_regs;

void vgic_init()
{
    // @ivanv: audit
    // TODO: fix for SMP
    for (int i = 0; i < NUM_SLOTS_SPI_VIRQ; i++) {
        vgic.vspis[i].virq = VIRQ_INVALID;
    }
    for (int i = 0; i < NUM_VCPU_LOCAL_VIRQS; i++) {
        vgic.vgic_vcpu[GUEST_BOOT_VCPU_ID].local_virqs[i].virq = VIRQ_INVALID;
    }
    for (int i = 0; i < NUM_LIST_REGS; i++) {
        vgic.vgic_vcpu[GUEST_BOOT_VCPU_ID].lr_shadow[i].virq = VIRQ_INVALID;
    }
    vgic.registers = &vgic_regs;
    vgic_regs.dist = &dist;
    vgic_regs.redist = &redist;

    vgic_dist_reset(&dist);
    vgic_redist_reset(&redist);
}
