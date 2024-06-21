/* @ivanv double check
 * Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <microkit.h>
#include "vgic.h"
#include "virq.h"
#include "fault.h"
#include "../../util/util.h"

#if defined(GIC_V2)
#include "vgic_v2.h"
#elif defined(GIC_V3)
#include "vgic_v3.h"
#else
#error "Unknown GIC version"
#endif

#include "vdist.h"

/* The driver expects the VGIC state to be initialised before calling any of the driver functionality. */
extern vgic_t vgic;

static bool vgic_handle_level(vgic_t *vgic, int vcpu_id,
                              struct virq_handle *irq)
{
    struct gic_dist_map *dist = vgic_get_dist(vgic->registers);

    /* Re-inject IRQ for level sensitive irq emulation, otherwise clear level */
    if (irq->level && !vgic_dist_is_edge_triggered(dist, irq->virq)) {
        return vgic_dist_set_pending_irq(vgic, vcpu_id, irq->virq);
    }

    irq->level = false;
    return true;
}

void vgic_irq_ack(vgic_t *vgic, int vcpu_id, struct virq_handle *irq)
{
    assert(vgic);
    assert(irq);

    virq_ack(vcpu_id, irq);

    /* Handle level sensitive irq emulation */
    if (!vgic_handle_level(vgic, vcpu_id, irq)) {
        printf("VGIC|ERROR: failed to handle level for %d\n", irq->virq);
    }
}

bool fault_handle_vgic_maintenance(size_t vcpu_id)
{
    // @ivanv: reivist, also inconsistency between int and bool
    bool success = true;
    int idx = microkit_mr_get(seL4_VGICMaintenance_IDX);
    /* Currently not handling spurious IRQs */
    // @ivanv: wtf, this comment seems irrelevant to the code.
    assert(idx >= 0);

    // @ivanv: Revisit and make sure it's still correct.
    vgic_vcpu_t *vgic_vcpu = get_vgic_vcpu(&vgic, vcpu_id);
    assert(vgic_vcpu);
    assert((idx >= 0) && (idx < ARRAY_SIZE(vgic_vcpu->lr_shadow)));
    struct virq_handle *slot = &vgic_vcpu->lr_shadow[idx];
    assert(slot->virq != VIRQ_INVALID);
    struct virq_handle lr_virq = *slot;
    slot->virq = VIRQ_INVALID;
    slot->ack_fn = NULL;
    slot->ack_data = NULL;
    /* Clear pending */
    LOG_IRQ("Maintenance IRQ %d\n", lr_virq.virq);
    set_pending(vgic_get_dist(vgic.registers), lr_virq.virq, false, vcpu_id);
    vgic_irq_ack(&vgic, vcpu_id, &lr_virq);
    /* Check the overflow list for pending IRQs */
    struct virq_handle *virq = vgic_irq_dequeue(&vgic, vcpu_id);

#if defined(GIC_V2)
    int group = 0;
#elif defined(GIC_V3)
    int group = 1;
#else
#error "Unknown GIC version"
#endif

    if (virq) {
        success = vgic_vcpu_load_list_reg(&vgic, vcpu_id, idx, group, virq);
    }

    if (!success) {
        printf("VGIC|ERROR: maintenance handler failed\n");
        assert(0);
    }

    return success;
}

// @ivanv: maybe this shouldn't be here?
bool vgic_register_irq(size_t vcpu_id, int virq_num, virq_ack_fn_t ack_fn, void *ack_data) {
    assert(virq_num >= 0 && virq_num != VIRQ_INVALID);
    struct virq_handle virq = {
        .virq = virq_num,
        .level = false,
        .ack_fn = ack_fn,
        .ack_data = ack_data,
    };

    return virq_add(vcpu_id, &vgic, &virq);
}

bool vgic_inject_irq(size_t vcpu_id, int irq)
{
    LOG_IRQ("Injecting IRQ %d\n", irq);

    return vgic_dist_set_pending_irq(&vgic, vcpu_id, irq);

    // @ivanv: explain why we don't check error before checking this fault stuff
    // @ivanv: seperately, it seems weird to have this fault handling code here?
    // @ivanv: revist
    // if (!fault_handled(vcpu->vcpu_arch.fault) && fault_is_wfi(vcpu->vcpu_arch.fault)) {
    //     // ignore_fault(vcpu->vcpu_arch.fault);
    //     err = advance_vcpu_fault(regs);
    // }
}

bool vgic_irq_set_level(size_t vcpu_id, int irq, bool level)
{
    bool changed;
    struct virq_handle *virq = virq_find_irq_data(&vgic, vcpu_id, irq);
    if (!virq) {
        printf("VGIC|ERROR: maintenance handler failed\n");
        return false;
    }

    changed = (virq->level != level);
    virq->level = level;

    /* inject only at rising edge */
    if (virq->level && changed) {
	return vgic_inject_irq(vcpu_id, irq);
    }

    return true;
}

// @ivanv: revisit this whole function
bool handle_vgic_dist_fault(size_t vcpu_id, uint64_t fault_addr, uint64_t fsr, seL4_UserContext *regs)
{
    /* Make sure that the fault address actually lies within the GIC distributor region. */
    assert(fault_addr >= GIC_DIST_PADDR);
    assert(fault_addr - GIC_DIST_PADDR < GIC_DIST_SIZE);

    uint64_t offset = fault_addr - GIC_DIST_PADDR;
    bool success = false;
    if (fault_is_read(fsr)) {
        // printf("VGIC|INFO: Read dist\n");
        success = vgic_dist_reg_read(vcpu_id, &vgic, offset, fsr, regs);
        assert(success);
    } else {
        // printf("VGIC|INFO: Write dist\n");
        success = vgic_dist_reg_write(vcpu_id, &vgic, offset, fsr, regs);
        assert(success);
    }

    return success;
}