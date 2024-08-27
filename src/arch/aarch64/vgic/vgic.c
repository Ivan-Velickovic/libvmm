/*
 * Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <microkit.h>
#include <libvmm/vcpu.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/aarch64/fault.h>
#include <libvmm/arch/aarch64/vgic/vgic.h>
#include <libvmm/arch/aarch64/vgic/virq.h>

#if defined(GIC_V2)
#include <libvmm/arch/aarch64/vgic/vgic_v2.h>
#elif defined(GIC_V3)
#include <libvmm/arch/aarch64/vgic/vgic_v3.h>
#else
#error "Unknown GIC version"
#endif

#include <libvmm/arch/aarch64/vgic/vdist.h>

/* The driver expects the VGIC state to be initialised before calling any of the driver functionality. */
extern vgic_t vgic;

bool vgic_handle_fault_maintenance(size_t vcpu_id)
{
    // @ivanv: reivist, also inconsistency between int and bool
    bool success = true;
    int idx = microkit_mr_get(seL4_VGICMaintenance_IDX);
    /* Currently not handling spurious IRQs */
    // @ivanv: this comment seems irrelevant to the code.
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
    virq_ack(vcpu_id, &lr_virq);
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
        LOG_VMM_ERR("vGIC maintenance handler failed\n");
        assert(0);
    }

    return success;
}

bool vgic_handle_fault_dist(size_t vcpu_id, size_t offset, size_t fsr, seL4_UserContext *regs, void *data)
{
    bool success = false;
    if (fault_is_read(fsr)) {
        success = vgic_handle_fault_dist_read(vcpu_id, &vgic, offset, fsr, regs);
        assert(success);
    } else {
        success = vgic_handle_fault_dist_write(vcpu_id, &vgic, offset, fsr, regs);
        assert(success);
    }

    return success;
}

bool vgic_register_irq(size_t vcpu_id, int virq_num, virq_ack_fn_t ack_fn, void *ack_data)
{
    assert(virq_num >= 0 && virq_num != VIRQ_INVALID);
    struct virq_handle virq = {
        .virq = virq_num,
        .ack_fn = ack_fn,
        .ack_data = ack_data,
    };

    return virq_add(vcpu_id, &vgic, &virq);
}

bool vgic_inject_irq(size_t vcpu_id, int irq)
{
    LOG_IRQ("(vCPU %d) injecting IRQ %d\n", vcpu_id, irq);

    bool success = vgic_dist_set_pending_irq(&vgic, vcpu_id, irq);
    assert(success);

    return success;
}
