/*
 * Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libvmm/vcpu.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/aarch64/fault.h>

/* GIC Distributor register access utilities */
#define GIC_DIST_REGN(offset, reg) ((offset-reg)/sizeof(uint32_t))
#define RANGE32(a, b) a ... b + (sizeof(uint32_t)-1)

#define IRQ_IDX(irq) ((irq) / 32)
#define IRQ_BIT(irq) (1U << ((irq) % 32))

static inline void set_sgi_ppi_pending(struct gic_dist_map *gic_dist, int irq, bool set_pending, int vcpu_id)
{
    if (set_pending) {
        gic_dist->pending_set0[vcpu_id] |= IRQ_BIT(irq);
        gic_dist->pending_clr0[vcpu_id] |= IRQ_BIT(irq);
    } else {
        gic_dist->pending_set0[vcpu_id] &= ~IRQ_BIT(irq);
        gic_dist->pending_clr0[vcpu_id] &= ~IRQ_BIT(irq);
    }
}

static inline void set_spi_pending(struct gic_dist_map *gic_dist, int irq, bool set_pending)
{
    if (set_pending) {
        gic_dist->pending_set[IRQ_IDX(irq)] |= IRQ_BIT(irq);
        gic_dist->pending_clr[IRQ_IDX(irq)] |= IRQ_BIT(irq);
    } else {
        gic_dist->pending_set[IRQ_IDX(irq)] &= ~IRQ_BIT(irq);
        gic_dist->pending_clr[IRQ_IDX(irq)] &= ~IRQ_BIT(irq);
    }
}

static inline void set_pending(struct gic_dist_map *gic_dist, int irq, bool set_pending, int vcpu_id)
{
    if (irq < NUM_VCPU_LOCAL_VIRQS) {
        set_sgi_ppi_pending(gic_dist, irq, set_pending, vcpu_id);
    } else {
        set_spi_pending(gic_dist, irq, set_pending);
    }
}

static inline bool is_sgi_ppi_pending(struct gic_dist_map *gic_dist, int irq, int vcpu_id)
{
    return !!(gic_dist->pending_set0[vcpu_id] & IRQ_BIT(irq));
}

static inline bool is_spi_pending(struct gic_dist_map *gic_dist, int irq)
{
    return !!(gic_dist->pending_set[IRQ_IDX(irq)] & IRQ_BIT(irq));
}

static inline bool is_pending(struct gic_dist_map *gic_dist, int irq, int vcpu_id)
{
    if (irq < NUM_VCPU_LOCAL_VIRQS) {
        return is_sgi_ppi_pending(gic_dist, irq, vcpu_id);
    } else {
        return is_spi_pending(gic_dist, irq);
    }
}

static inline void set_sgi_ppi_enable(struct gic_dist_map *gic_dist, int irq, bool set_enable, int vcpu_id)
{
    if (set_enable) {
        gic_dist->enable_set0[vcpu_id] |= IRQ_BIT(irq);
        gic_dist->enable_clr0[vcpu_id] |= IRQ_BIT(irq);
    } else {
        gic_dist->enable_set0[vcpu_id] &= ~IRQ_BIT(irq);
        gic_dist->enable_clr0[vcpu_id] &= ~IRQ_BIT(irq);
    }
}

static inline void set_spi_enable(struct gic_dist_map *gic_dist, int irq, bool set_enable)
{
    if (set_enable) {
        gic_dist->enable_set[IRQ_IDX(irq)] |= IRQ_BIT(irq);
        gic_dist->enable_clr[IRQ_IDX(irq)] |= IRQ_BIT(irq);
    } else {
        gic_dist->enable_set[IRQ_IDX(irq)] &= ~IRQ_BIT(irq);
        gic_dist->enable_clr[IRQ_IDX(irq)] &= ~IRQ_BIT(irq);
    }
}

static inline void set_enable(struct gic_dist_map *gic_dist, int irq, bool set_enable, int vcpu_id)
{
    if (irq < NUM_VCPU_LOCAL_VIRQS) {
        set_sgi_ppi_enable(gic_dist, irq, set_enable, vcpu_id);
    } else {
        set_spi_enable(gic_dist, irq, set_enable);
    }
}

static inline bool is_sgi_ppi_enabled(struct gic_dist_map *gic_dist, int irq, int vcpu_id)
{
    return !!(gic_dist->enable_set0[vcpu_id] & IRQ_BIT(irq));
}

static inline bool is_spi_enabled(struct gic_dist_map *gic_dist, int irq)
{
    return !!(gic_dist->enable_set[IRQ_IDX(irq)] & IRQ_BIT(irq));
}

static inline bool is_enabled(struct gic_dist_map *gic_dist, int irq, int vcpu_id)
{
    if (irq < NUM_VCPU_LOCAL_VIRQS) {
        return is_sgi_ppi_enabled(gic_dist, irq, vcpu_id);
    } else {
        return is_spi_enabled(gic_dist, irq);
    }
}

static inline bool is_sgi_ppi_active(struct gic_dist_map *gic_dist, int irq, int vcpu_id)
{
    return !!(gic_dist->active0[vcpu_id] & IRQ_BIT(irq));
}

static inline bool is_spi_active(struct gic_dist_map *gic_dist, int irq)
{
    return !!(gic_dist->active[IRQ_IDX(irq)] & IRQ_BIT(irq));
}

static inline bool is_active(struct gic_dist_map *gic_dist, int irq, int vcpu_id)
{
    if (irq < NUM_VCPU_LOCAL_VIRQS) {
        return is_sgi_ppi_active(gic_dist, irq, vcpu_id);
    } else {
        return is_spi_active(gic_dist, irq);
    }
}

static void vgic_dist_enable_irq(vgic_t *vgic, size_t vcpu_id, int irq)
{
    LOG_DIST("Enabling IRQ %d\n", irq);
    set_enable(vgic_get_dist(vgic->registers), irq, true, vcpu_id);
    struct virq_handle *virq_data = virq_find_irq_data(vgic, vcpu_id, irq);
    // assert(virq_data != NULL);
    // @ivanv: explain
    if (!virq_data) {
        return;
    }
    if (virq_data->virq != VIRQ_INVALID) {
        /* STATE b) */
        if (!is_pending(vgic_get_dist(vgic->registers), virq_data->virq, vcpu_id)) {
            virq_ack(vcpu_id, virq_data);
        }
    } else {
        LOG_DIST("Enabled IRQ %d has no handle\n", irq);
    }
}

static void vgic_dist_disable_irq(vgic_t *vgic, size_t vcpu_id, int irq)
{
    /* STATE g)
     *
     * It is IMPLEMENTATION DEFINED if a GIC allows disabling SGIs. Our vGIC
     * implementation does not allows it, such requests are simply ignored.
     * Since it is not uncommon that a guest OS tries disabling SGIs, e.g. as
     * part of the platform initialization, no dedicated messages are logged
     * here to avoid bloating the logs.
     */
    if (irq >= NUM_SGI_VIRQS) {
        LOG_DIST("Disabling IRQ %d\n", irq);
        set_enable(vgic_get_dist(vgic->registers), irq, false, vcpu_id);
    }
}

static bool vgic_dist_set_pending_irq(vgic_t *vgic, size_t vcpu_id, int irq)
{
    /* STATE c) */
    /* First check that we find vIRQ data in case the vIRQ has not been
     * registered yet. */
    struct virq_handle *virq_data = virq_find_irq_data(vgic, vcpu_id, irq);
    assert(virq_data);
    if (!virq_data) {
        LOG_VMM_ERR("could not find vIRQ data for vIRQ 0x%lx on vCPU 0x%lx\n", irq, vcpu_id);
        return false;
    }
    struct gic_dist_map *dist = vgic_get_dist(vgic->registers);

    if (virq_data->virq == VIRQ_INVALID || !vgic_dist_is_enabled(dist) || !is_enabled(dist, irq, vcpu_id)) {
        if (virq_data->virq == VIRQ_INVALID) {
            LOG_VMM_ERR("vIRQ data could not be found for IRQ 0x%lx\n", irq);
        }
        if (!vgic_dist_is_enabled(dist)) {
            LOG_VMM_ERR("vGIC distributor is not enabled for IRQ 0x%lx\n", irq);
        }
        if (!is_enabled(dist, irq, vcpu_id)) {
            LOG_VMM_ERR("vIRQ 0x%lx is not enabled\n", irq);
        }
        return false;
    }

    if (is_pending(dist, virq_data->virq, vcpu_id)) {
        // Do nothing if it's already pending
        return true;
    }

    LOG_DIST("Pending set: Inject IRQ from pending set (%d)\n", irq);
    set_pending(dist, virq_data->virq, true, vcpu_id);

    /* Enqueueing an IRQ and dequeueing it right after makes little sense
     * now, but in the future this is needed to support IRQ priorities.
     */
    bool success = vgic_irq_enqueue(vgic, vcpu_id, virq_data);
    if (!success) {
        LOG_VMM_ERR("Failure enqueueing IRQ, increase MAX_IRQ_QUEUE_LEN");
        assert(0);
        return false;
    }

    int idx = vgic_find_empty_list_reg(vgic, vcpu_id);
    if (idx < 0) {
        /* There were no empty list registers available, but that's not a big
         * deal -- we have already enqueued this IRQ and eventually the vGIC
         * maintenance code will load it to a list register from the queue.
         */
        return true;
    }

    struct virq_handle *virq = vgic_irq_dequeue(vgic, vcpu_id);
    assert(virq->virq != VIRQ_INVALID);

#if defined(GIC_V2)
    int group = 0;
#elif defined(GIC_V3)
    int group = 1;
#else
#error "Unknown GIC version"
#endif

    // @ivanv: I don't understand why GIC v2 is group 0 and GIC v3 is group 1.
    return vgic_vcpu_load_list_reg(vgic, vcpu_id, idx, group, virq);
}

static void vgic_dist_clr_pending_irq(struct gic_dist_map *dist, size_t vcpu_id, int irq)
{
    LOG_DIST("Clear pending IRQ %d\n", irq);
    set_pending(dist, irq, false, vcpu_id);
    /* TODO: remove from IRQ queue and list registers as well */
    // @ivanv
}

static bool vgic_handle_fault_dist_read(size_t vcpu_id, vgic_t *vgic, uint64_t offset, uint64_t fsr,
                                        seL4_UserContext *regs)
{
    struct gic_dist_map *gic_dist = vgic_get_dist(vgic->registers);
    uint32_t reg = 0;
    int reg_offset = 0;
    uintptr_t base_reg;
    uint32_t *reg_ptr;
    switch (offset) {
    case RANGE32(GIC_DIST_CTLR, GIC_DIST_CTLR):
        reg = gic_dist->ctlr;
        break;
    case RANGE32(GIC_DIST_TYPER, GIC_DIST_TYPER):
        reg = gic_dist->typer;
        break;
    case RANGE32(GIC_DIST_IIDR, GIC_DIST_IIDR):
        reg = gic_dist->iidr;
        break;
    case RANGE32(0x00C, 0x01C):
        /* Reserved */
        break;
    case RANGE32(0x020, 0x03C):
        /* IMPLEMENTATION DEFINED registers. */
        break;
    case RANGE32(0x040, 0x07C):
        /* Reserved */
        break;
    case RANGE32(GIC_DIST_IGROUPR0, GIC_DIST_IGROUPR0):
        reg = gic_dist->irq_group0[vcpu_id];
        break;
    case RANGE32(GIC_DIST_IGROUPR1, GIC_DIST_IGROUPRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_IGROUPR1);
        reg = gic_dist->irq_group[reg_offset];
        break;
    case RANGE32(GIC_DIST_ISENABLER0, GIC_DIST_ISENABLER0):
        reg = gic_dist->enable_set0[vcpu_id];
        break;
    case RANGE32(GIC_DIST_ISENABLER1, GIC_DIST_ISENABLERN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ISENABLER1);
        reg = gic_dist->enable_set[reg_offset];
        break;
    case RANGE32(GIC_DIST_ICENABLER0, GIC_DIST_ICENABLER0):
        reg = gic_dist->enable_clr0[vcpu_id];
        break;
    case RANGE32(GIC_DIST_ICENABLER1, GIC_DIST_ICENABLERN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ICENABLER1);
        reg = gic_dist->enable_clr[reg_offset];
        break;
    case RANGE32(GIC_DIST_ISPENDR0, GIC_DIST_ISPENDR0):
        reg = gic_dist->pending_set0[vcpu_id];
        break;
    case RANGE32(GIC_DIST_ISPENDR1, GIC_DIST_ISPENDRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ISPENDR1);
        reg = gic_dist->pending_set[reg_offset];
        break;
    case RANGE32(GIC_DIST_ICPENDR0, GIC_DIST_ICPENDR0):
        reg = gic_dist->pending_clr0[vcpu_id];
        break;
    case RANGE32(GIC_DIST_ICPENDR1, GIC_DIST_ICPENDRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ICPENDR1);
        reg = gic_dist->pending_clr[reg_offset];
        break;
    case RANGE32(GIC_DIST_ISACTIVER0, GIC_DIST_ISACTIVER0):
        reg = gic_dist->active0[vcpu_id];
        break;
    case RANGE32(GIC_DIST_ISACTIVER1, GIC_DIST_ISACTIVERN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ISACTIVER1);
        reg = gic_dist->active[reg_offset];
        break;
    case RANGE32(GIC_DIST_ICACTIVER0, GIC_DIST_ICACTIVER0):
        reg = gic_dist->active_clr0[vcpu_id];
        break;
    case RANGE32(GIC_DIST_ICACTIVER1, GIC_DIST_ICACTIVERN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ICACTIVER1);
        reg = gic_dist->active_clr[reg_offset];
        break;
    case RANGE32(GIC_DIST_IPRIORITYR0, GIC_DIST_IPRIORITYR7):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_IPRIORITYR0);
        reg = gic_dist->priority0[vcpu_id][reg_offset];
        break;
    case RANGE32(GIC_DIST_IPRIORITYR8, GIC_DIST_IPRIORITYRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_IPRIORITYR8);
        reg = gic_dist->priority[reg_offset];
        break;
    case RANGE32(0x7FC, 0x7FC):
        /* Reserved */
        break;
    case RANGE32(GIC_DIST_ITARGETSR0, GIC_DIST_ITARGETSR7):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ITARGETSR0);
        reg = gic_dist->targets0[vcpu_id][reg_offset];
        break;
    case RANGE32(GIC_DIST_ITARGETSR8, GIC_DIST_ITARGETSRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ITARGETSR8);
        reg = gic_dist->targets[reg_offset];
        break;
    case RANGE32(0xBFC, 0xBFC):
        /* Reserved */
        break;
    case RANGE32(GIC_DIST_ICFGR0, GIC_DIST_ICFGRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ICFGR0);
        reg = gic_dist->config[reg_offset];
        break;
#if defined(GIC_V2)
    case RANGE32(0xD00, 0xDE4):
        /* IMPLEMENTATION DEFINED registers. */
        base_reg = (uintptr_t) & (gic_dist->spi[0]);
        reg_ptr = (uint32_t *)(base_reg + (offset - 0xD00));
        reg = *reg_ptr;
        break;
#endif
    case RANGE32(0xDE8, 0xEFC):
        /* Reserved [0xDE8 - 0xE00) */
        /* GIC_DIST_NSACR [0xE00 - 0xF00) - Not supported */
        break;
    case RANGE32(GIC_DIST_SGIR, GIC_DIST_SGIR):
        reg = gic_dist->sgir;
        break;
    case RANGE32(0xF04, 0xF0C):
        /* Reserved */
        break;
    case RANGE32(GIC_DIST_CPENDSGIR0, GIC_DIST_CPENDSGIRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_CPENDSGIR0);
        reg = gic_dist->sgi_pending_clr[vcpu_id][reg_offset];
        break;
    case RANGE32(GIC_DIST_SPENDSGIR0, GIC_DIST_SPENDSGIRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_SPENDSGIR0);
        reg = gic_dist->sgi_pending_set[vcpu_id][reg_offset];
        break;
    case RANGE32(0xF30, 0xFBC):
        /* Reserved */
        break;
#if defined(GIC_V2)
    // @ivanv: understand why this is GIC v2 specific and make a command so others can understand as well.
    case RANGE32(0xFC0, 0xFFB):
        base_reg = (uintptr_t) & (gic_dist->periph_id[0]);
        reg_ptr = (uint32_t *)(base_reg + (offset - 0xFC0));
        reg = *reg_ptr;
        break;
#endif
#if defined(GIC_V3)
    // @ivanv: Understand and comment GICv3 specific stuff
    case RANGE32(0x6100, 0x7F00):
        base_reg = (uintptr_t) & (gic_dist->irouter[0]);
        reg_ptr = (uint32_t *)(base_reg + (offset - 0x6100));
        reg = *reg_ptr;
        break;

    case RANGE32(0xFFD0, 0xFFFC):
        base_reg = (uintptr_t) & (gic_dist->pidrn[0]);
        reg_ptr = (uint32_t *)(base_reg + (offset - 0xFFD0));
        reg = *reg_ptr;
        break;
#endif
    default:
        LOG_VMM_ERR("Unknown register offset 0x%x", offset);
        /* Pretend that everything is fine */
        return true;
    }
    uint32_t mask = fault_get_data_mask(GIC_DIST_PADDR + offset, fsr);
    fault_emulate_write(regs, GIC_DIST_PADDR + offset, fsr, reg & mask);

    return true;
}

static inline void emulate_reg_write_access(seL4_UserContext *regs, uint64_t addr, uint64_t fsr, uint32_t *reg)
{
    *reg = fault_emulate(regs, *reg, addr, fsr, fault_get_data(regs, fsr));
}

static bool vgic_handle_fault_dist_write(size_t vcpu_id, vgic_t *vgic, uint64_t offset, uint64_t fsr,
                                         seL4_UserContext *regs)
{
    bool success = true;
    struct gic_dist_map *gic_dist = vgic_get_dist(vgic->registers);
    uint64_t addr = GIC_DIST_PADDR + offset;
    uint32_t mask = fault_get_data_mask(addr, fsr);
    uint32_t reg_offset = 0;
    uint32_t data;
    switch (offset) {
    case RANGE32(GIC_DIST_CTLR, GIC_DIST_CTLR):
        data = fault_get_data(regs, fsr);
        if (data == GIC_ENABLED) {
            vgic_dist_enable(gic_dist);
        } else if (data == 0) {
            vgic_dist_disable(gic_dist);
        } else {
            LOG_VMM_ERR("Unknown enable register encoding");
            // @ivanv: goto ignore fault?
        }
        break;
    case RANGE32(GIC_DIST_TYPER, GIC_DIST_TYPER):
        /* TYPER provides information about the GIC configuration, we do not do
         * anything here as there should be no reason for the guest to write to
         * this register.
         */
        break;
    case RANGE32(GIC_DIST_IIDR, GIC_DIST_IIDR):
        /* IIDR provides information about the distributor's implementation, we
         * do not do anything here as there should be no reason for the guest
         * to write to this register.
         */
        break;
    case RANGE32(0x00C, 0x01C):
        /* Reserved */
        break;
    case RANGE32(0x020, 0x03C):
        /* IMPLEMENTATION DEFINED registers. */
        break;
    case RANGE32(0x040, 0x07C):
        /* Reserved */
        break;
    case RANGE32(GIC_DIST_IGROUPR0, GIC_DIST_IGROUPR0):
        emulate_reg_write_access(regs, addr, fsr, &gic_dist->irq_group0[vcpu_id]);
        break;
    case RANGE32(GIC_DIST_IGROUPR1, GIC_DIST_IGROUPRN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_IGROUPR1);
        emulate_reg_write_access(regs, addr, fsr, &gic_dist->irq_group[reg_offset]);
        break;
    case RANGE32(GIC_DIST_ISENABLER0, GIC_DIST_ISENABLERN):
        data = fault_get_data(regs, fsr);
        /* Mask the data to write */
        data &= mask;
        while (data) {
            int irq;
            irq = CTZ(data);
            data &= ~(1U << irq);
            irq += (offset - GIC_DIST_ISENABLER0) * 8;
            vgic_dist_enable_irq(vgic, vcpu_id, irq);
        }
        break;
    case RANGE32(GIC_DIST_ICENABLER0, GIC_DIST_ICENABLERN):
        data = fault_get_data(regs, fsr);
        /* Mask the data to write */
        data &= mask;
        while (data) {
            int irq;
            irq = CTZ(data);
            data &= ~(1U << irq);
            irq += (offset - GIC_DIST_ICENABLER0) * 8;
            vgic_dist_disable_irq(vgic, vcpu_id, irq);
        }
        break;
    case RANGE32(GIC_DIST_ISPENDR0, GIC_DIST_ISPENDRN):
        data = fault_get_data(regs, fsr);
        /* Mask the data to write */
        data &= mask;
        while (data) {
            int irq;
            irq = CTZ(data);
            data &= ~(1U << irq);
            irq += (offset - GIC_DIST_ISPENDR0) * 8;
            success = vgic_dist_set_pending_irq(vgic, vcpu_id, irq);
            assert(success);
        }
        break;
    case RANGE32(GIC_DIST_ICPENDR0, GIC_DIST_ICPENDRN):
        data = fault_get_data(regs, fsr);
        /* Mask the data to write */
        data &= mask;
        while (data) {
            int irq;
            irq = CTZ(data);
            data &= ~(1U << irq);
            irq += (offset - GIC_DIST_ICPENDR0) * 8;
            vgic_dist_clr_pending_irq(gic_dist, vcpu_id, irq);
        }
        break;
    case RANGE32(GIC_DIST_ISACTIVER0, GIC_DIST_ISACTIVER0):
        emulate_reg_write_access(regs, addr, fsr, &gic_dist->active0[vcpu_id]);
        break;
    case RANGE32(GIC_DIST_ISACTIVER1, GIC_DIST_ISACTIVERN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ISACTIVER1);
        emulate_reg_write_access(regs, addr, fsr, &gic_dist->active[reg_offset]);
        break;
    case RANGE32(GIC_DIST_ICACTIVER0, GIC_DIST_ICACTIVER0):
        emulate_reg_write_access(regs, addr, fsr, &gic_dist->active_clr0[vcpu_id]);
        break;
    case RANGE32(GIC_DIST_ICACTIVER1, GIC_DIST_ICACTIVERN):
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ICACTIVER1);
        emulate_reg_write_access(regs, addr, fsr, &gic_dist->active_clr[reg_offset]);
        break;
    case RANGE32(GIC_DIST_IPRIORITYR0, GIC_DIST_IPRIORITYRN):
        break;
    case RANGE32(0x7FC, 0x7FC):
        /* Reserved */
        break;
    case RANGE32(GIC_DIST_ITARGETSR0, GIC_DIST_ITARGETSRN):
        break;
    case RANGE32(0xBFC, 0xBFC):
        /* Reserved */
        break;
    case RANGE32(GIC_DIST_ICFGR0, GIC_DIST_ICFGRN):
        /*
         * Emulate accesses to interrupt configuration registers to set the IRQ
         * to be edge-triggered or level-sensitive.
         */
        reg_offset = GIC_DIST_REGN(offset, GIC_DIST_ICFGR0);
        emulate_reg_write_access(regs, addr, fsr, &gic_dist->config[reg_offset]);
        break;
    case RANGE32(0xD00, 0xDFC):
        /* IMPLEMENTATION DEFINED registers. */
        break;
    case RANGE32(0xE00, 0xEFC):
        /* GIC_DIST_NSACR [0xE00 - 0xF00) - Not supported */
        break;
    case RANGE32(GIC_DIST_SGIR, GIC_DIST_SGIR):
        data = fault_get_data(regs, fsr);
        int mode = (data & GIC_DIST_SGI_TARGET_LIST_FILTER_MASK) >> GIC_DIST_SGI_TARGET_LIST_FILTER_SHIFT;
        int virq = (data & GIC_DIST_SGI_INTID_MASK);
        uint16_t target_list = 0;
        switch (mode) {
        case GIC_DIST_SGI_TARGET_LIST_SPEC:
            /* Forward VIRQ to VCPUs specified in CPUTargetList */
            target_list = (data & GIC_DIST_SGI_CPU_TARGET_LIST_MASK) >> GIC_DIST_SGI_CPU_TARGET_LIST_SHIFT;
            break;
        case GIC_DIST_SGI_TARGET_LIST_OTHERS:
            /* Forward virq to all VCPUs except the requesting VCPU */
            target_list = (1 << GUEST_NUM_VCPUS) - 1;
            target_list = target_list & ~(1 << vcpu_id);
            break;
        case GIC_DIST_SGI_TARGET_SELF:
            /* Forward to virq to only the requesting vcpu */
            target_list = (1 << vcpu_id);
            break;
        default:
            LOG_VMM_ERR("Unknown SGIR Target List Filter mode");
            goto ignore_fault;
        }
        for (int i = 0; i < GUEST_NUM_VCPUS; i++) {
            if ((1 << i) & target_list && vcpu_is_on(i)) {
                success = vgic_inject_irq(i, virq);
                assert(success);
                if (!success) {
                    return false;
                }
            }
        }
        break;
    case RANGE32(0xF04, 0xF0C):
        /* Reserved */
        break;
    case RANGE32(GIC_DIST_CPENDSGIR0, GIC_DIST_SPENDSGIRN):
        // @ivanv: come back to
        assert(!"vgic SGI reg not implemented!\n");
        break;
    case RANGE32(0xF30, 0xFBC):
        /* Reserved */
        break;
    case RANGE32(0xFC0, 0xFFC):
        // @ivanv: GICv2 specific, GICv3 has different range for impl defined registers.
        /* IMPLEMENTATION DEFINED registers. */
        break;
#if defined(GIC_V3)
    // @ivanv: explain GICv3 specific stuff, and also don't use the hardcoded valuees
    case RANGE32(0x6100, 0x7F00):
        // @ivanv revisit
        // data = fault_get_data(fault);
        // ZF_LOGF_IF(data, "bad dist: 0x%x 0x%x", offset, data);
        break;
#endif
    default:
        LOG_VMM_ERR("Unknown register offset 0x%x", offset);
        assert(0);
    }
ignore_fault:
    assert(success);
    if (!success) {
        return false;
    }

    return success;
}
