/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <microkit.h>
#include <libvmm/virq.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/aarch64/fault.h>
#include <libvmm/arch/aarch64/vgic/vgic.h>

/* Maps Microkit channel numbers with registered vIRQ */
int virq_passthrough_map[MAX_PASSTHROUGH_IRQ] = {-1};

#define SGI_RESCHEDULE_IRQ  0
#define SGI_FUNC_CALL       1
#define PPI_VTIMER_IRQ      27

static void vppi_event_ack(size_t vcpu_id, int irq, void *cookie)
{
    microkit_vcpu_arm_ack_vppi(vcpu_id, irq);
}

static void sgi_ack(size_t vcpu_id, int irq, void *cookie) {}

bool virq_controller_init()
{
    bool success;

    vgic_init();
#if defined(GIC_V2)
    LOG_VMM("initialised virtual GICv2 driver\n");
#elif defined(GIC_V3)
    LOG_VMM("initialised virtual GICv3 driver\n");
#else
#error "Unsupported GIC version"
#endif

    /* Register the fault handler */
    success = fault_register_vm_exception_handler(GIC_DIST_PADDR, GIC_DIST_SIZE, vgic_handle_fault_dist, NULL);
    if (!success) {
        LOG_VMM_ERR("Failed to register fault handler for GIC distributor region\n");
        return false;
    }
#if defined(GIC_V3)
    success = fault_register_vm_exception_handler(GIC_REDIST_PADDR, GIC_REDIST_SIZE, vgic_handle_fault_redist, NULL);
    if (!success) {
        LOG_VMM_ERR("Failed to register fault handler for GIC redistributor region\n");
        return false;
    }
#endif

    for (int vcpu = 0; vcpu < GUEST_NUM_VCPUS; vcpu++) {
        success = vgic_register_irq(vcpu, PPI_VTIMER_IRQ, &vppi_event_ack, NULL);
        if (!success) {
            LOG_VMM_ERR("Failed to register vCPU virtual timer IRQ: 0x%lx\n", PPI_VTIMER_IRQ);
            return false;
        }
        success = vgic_register_irq(vcpu, SGI_RESCHEDULE_IRQ, &sgi_ack, NULL);
        if (!success) {
            LOG_VMM_ERR("Failed to register vCPU SGI 0 IRQ");
            return false;
        }
        success = vgic_register_irq(vcpu, SGI_FUNC_CALL, &sgi_ack, NULL);
        if (!success) {
            LOG_VMM_ERR("Failed to register vCPU SGI 1 IRQ");
            return false;
        }
    }

    return true;
}

bool virq_inject_vcpu(size_t vcpu_id, int irq)
{
    return vgic_inject_irq(vcpu_id, irq);
}

bool virq_inject(int irq)
{
    return vgic_inject_irq(GUEST_BOOT_VCPU_ID, irq);
}

bool virq_register(size_t vcpu_id, size_t virq_num, virq_ack_fn_t ack_fn, void *ack_data)
{
    return vgic_register_irq(vcpu_id, virq_num, ack_fn, ack_data);
}

static void virq_passthrough_ack(size_t vcpu_id, int irq, void *cookie)
{
    /* We are down-casting to microkit_channel so must first cast to size_t */
    microkit_irq_ack((microkit_channel)(size_t)cookie);
}

bool virq_register_passthrough(size_t vcpu_id, size_t irq, microkit_channel irq_ch)
{
    assert(irq_ch < MICROKIT_MAX_CHANNELS);
    if (irq_ch >= MICROKIT_MAX_CHANNELS) {
        LOG_VMM_ERR("Invalid channel number given '0x%lx' for passthrough vIRQ 0x%lx\n", irq_ch, irq);
        return false;
    }

    LOG_VMM("Register passthrough vIRQ 0x%lx on vCPU 0x%lx (IRQ channel: 0x%lx)\n", irq, vcpu_id, irq_ch);
    virq_passthrough_map[irq_ch] = irq;

    bool success = virq_register(GUEST_BOOT_VCPU_ID, irq, &virq_passthrough_ack, (void *)(size_t)irq_ch);
    assert(success);
    if (!success) {
        LOG_VMM_ERR("Failed to register passthrough vIRQ %d\n", irq);
        return false;
    }

    return true;
}

bool virq_handle_passthrough(microkit_channel irq_ch)
{
    assert(virq_passthrough_map[irq_ch] >= 0);
    if (virq_passthrough_map[irq_ch] < 0) {
        LOG_VMM_ERR("attempted to handle invalid passthrough IRQ channel 0x%lx\n", irq_ch);
        return false;
    }

    bool success = vgic_inject_irq(GUEST_BOOT_VCPU_ID, virq_passthrough_map[irq_ch]);
    if (!success) {
        LOG_VMM_ERR("could not inject passthrough vIRQ 0x%lx, dropped on vCPU 0x%lx\n", virq_passthrough_map[irq_ch],
                    GUEST_BOOT_VCPU_ID);
        return false;
    }

    return true;
}
