/*
 * Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * This file controls and maintains the virtualised GICv2 device for the VM. The spec used is: @ivanv
 * IRQs must be registered at initialisation using the function: vm_register_irq
 *
 * @ivanv: talk about what features this driver actually implements
 * This driver supports multiple vCPUs.
 *
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
#include <microkit.h>

#include <libvmm/virq.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/aarch64/fault.h>
#include <libvmm/arch/aarch64/vgic/vgic.h>
#include <libvmm/arch/aarch64/vgic/vgic_v2.h>
#include <libvmm/arch/aarch64/vgic/virq.h>
#include <libvmm/arch/aarch64/vgic/vdist.h>

vgic_t vgic;
struct gic_dist_map dist;

static void vgic_dist_reset(struct gic_dist_map *gic_dist)
{
    gic_dist->typer = 0x0000fce7; /* RO */
    gic_dist->iidr = 0x0200043b; /* RO */

    for (int i = 0; i < GUEST_NUM_VCPUS; i++) {
        gic_dist->enable_set0[i] = 0x0000ffff; /* 16bit RO */
        gic_dist->enable_clr0[i] = 0x0000ffff; /* 16bit RO */
    }

    /* Reset value depends on GIC configuration */
    gic_dist->config[0]       = 0xaaaaaaaa; /* RO */
    gic_dist->config[1]       = 0x55540000;
    gic_dist->config[2]       = 0x55555555;
    gic_dist->config[3]       = 0x55555555;
    gic_dist->config[4]       = 0x55555555;
    gic_dist->config[5]       = 0x55555555;
    gic_dist->config[6]       = 0x55555555;
    gic_dist->config[7]       = 0x55555555;
    gic_dist->config[8]       = 0x55555555;
    gic_dist->config[9]       = 0x55555555;
    gic_dist->config[10]      = 0x55555555;
    gic_dist->config[11]      = 0x55555555;
    gic_dist->config[12]      = 0x55555555;
    gic_dist->config[13]      = 0x55555555;
    gic_dist->config[14]      = 0x55555555;
    gic_dist->config[15]      = 0x55555555;

    /* Configure per-processor SGI/PPI target registers */
    for (int i = 0; i < GUEST_NUM_VCPUS; i++) {
        for (int j = 0; j < ARRAY_SIZE(gic_dist->targets0[i]); j++) {
            for (int irq = 0; irq < sizeof(uint32_t); irq++) {
                gic_dist->targets0[i][j] |= ((1 << i) << (irq * 8));
            }
        }
    }
    /* Deliver the SPI interrupts to the first CPU interface */
    for (int i = 0; i < ARRAY_SIZE(gic_dist->targets); i++) {
        gic_dist->targets[i] = 0x1010101;
    }

    /* identification */
    gic_dist->periph_id[4]    = 0x00000004; /* RO */
    gic_dist->periph_id[8]    = 0x00000090; /* RO */
    gic_dist->periph_id[9]    = 0x000000b4; /* RO */
    gic_dist->periph_id[10]   = 0x0000002b; /* RO */
    gic_dist->component_id[0] = 0x0000000d; /* RO */
    gic_dist->component_id[1] = 0x000000f0; /* RO */
    gic_dist->component_id[2] = 0x00000005; /* RO */
    gic_dist->component_id[3] = 0x000000b1; /* RO */
}

void vgic_init()
{
    memset(&vgic, 0, sizeof(vgic_t));

    for (int vcpu = 0; vcpu < GUEST_NUM_VCPUS; vcpu++) {
        for (int i = 0; i < NUM_VCPU_LOCAL_VIRQS; i++) {
            vgic.vgic_vcpu[vcpu].local_virqs[i].virq = VIRQ_INVALID;
        }
        for (int i = 0; i < NUM_LIST_REGS; i++) {
            vgic.vgic_vcpu[vcpu].lr_shadow[i].virq = VIRQ_INVALID;
        }
        for (int i = 0; i < MAX_IRQ_QUEUE_LEN; i++) {
            vgic.vgic_vcpu[vcpu].irq_queue.irqs[i] = NULL;
        }
        for (int i = 0; i < NUM_SLOTS_SPI_VIRQ; i++) {
            vgic.vspis[i].virq = VIRQ_INVALID;
            vgic.vspis[i].ack_fn = NULL;
            vgic.vspis[i].ack_data = NULL;
        }
    }

    vgic.registers = &dist;
    memset(vgic.registers, 0, sizeof(struct gic_dist_map));
    vgic_dist_reset(vgic_get_dist(vgic.registers));
}
