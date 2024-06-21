/*
 * Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include "util.h"

#define GIC_ENABLED 1

/*
 * This the memory map for the GIC distributor.
 *
 * You will note that some of these registers in the memory map are actually
 * arrays with the length of the number of virtual CPUs that the guest has.
 * The reason for this has to do with the fact that we are virtualising the GIC
 * distributor. In actual hardware, these registers would be banked, meaning
 * that the value of each register depends on the CPU that is accessing it.
 * However, since we are dealing with virutal CPUs, we have to make the
 * registers appear as if they are banked. You should note that the commented
 * address offsets listed on the right of each register are the offset from the
 * *guest's* perspective.
 *
 */
struct gic_dist_map {
    uint32_t ctlr;                                  /* 0x000 */
    uint32_t typer;                                 /* 0x004 */
    uint32_t iidr;                                  /* 0x008 */

    uint32_t res1[29];                              /* [0x00C, 0x080) */

    uint32_t irq_group0[GUEST_NUM_VCPUS];           /* [0x080, 0x84) */
    uint32_t irq_group[31];                         /* [0x084, 0x100) */
    uint32_t enable_set0[GUEST_NUM_VCPUS];          /* [0x100, 0x104) */
    uint32_t enable_set[31];                        /* [0x104, 0x180) */
    uint32_t enable_clr0[GUEST_NUM_VCPUS];          /* [0x180, 0x184) */
    uint32_t enable_clr[31];                        /* [0x184, 0x200) */
    uint32_t pending_set0[GUEST_NUM_VCPUS];         /* [0x200, 0x204) */
    uint32_t pending_set[31];                       /* [0x204, 0x280) */
    uint32_t pending_clr0[GUEST_NUM_VCPUS];         /* [0x280, 0x284) */
    uint32_t pending_clr[31];                       /* [0x284, 0x300) */
    uint32_t active0[GUEST_NUM_VCPUS];              /* [0x300, 0x304) */
    uint32_t active[31];                            /* [0x300, 0x380) */
    uint32_t active_clr0[GUEST_NUM_VCPUS];          /* [0x380, 0x384) */
    uint32_t active_clr[31];                        /* [0x384, 0x400) */
    uint32_t priority0[GUEST_NUM_VCPUS][8];         /* [0x400, 0x420) */
    uint32_t priority[247];                         /* [0x420, 0x7FC) */
    uint32_t res3;                                  /* 0x7FC */

    uint32_t targets0[GUEST_NUM_VCPUS][8];          /* [0x800, 0x820) */
    uint32_t targets[247];                          /* [0x820, 0xBFC) */
    uint32_t res4;                                  /* 0xBFC */

    uint32_t config[64];                            /* [0xC00, 0xD00) */

    uint32_t spi[32];                               /* [0xD00, 0xD80) */
    uint32_t res5[20];                              /* [0xD80, 0xDD0) */
    uint32_t res6;                                  /* 0xDD0 */
    uint32_t legacy_int;                            /* 0xDD4 */
    uint32_t res7[2];                               /* [0xDD8, 0xDE0) */
    uint32_t match_d;                               /* 0xDE0 */
    uint32_t enable_d;                              /* 0xDE4 */
    uint32_t res8[70];                              /* [0xDE8, 0xF00) */

    uint32_t sgir;                                  /* 0xF00 */
    uint32_t res9[3];                               /* [0xF04, 0xF10) */

    uint32_t sgi_pending_clr[GUEST_NUM_VCPUS][4];   /* [0xF10, 0xF20) */
    uint32_t sgi_pending_set[GUEST_NUM_VCPUS][4];   /* [0xF20, 0xF30) */
    uint32_t res10[40];                             /* [0xF30, 0xFC0) */

    uint32_t periph_id[12];                         /* [0xFC0, 0xFF0) */
    uint32_t component_id[4];                       /* [0xFF0, 0xFFF] */
};

/*
 * GIC Distributor Register Map
 * ARM Generic Interrupt Controller (Architecture version 2.0)
 * Architecture Specification (Issue B.b)
 * Chapter 4 Programmers' Model - Table 4.1
 */
#define GIC_DIST_CTLR           0x000
#define GIC_DIST_TYPER          0x004
#define GIC_DIST_IIDR           0x008
#define GIC_DIST_IGROUPR0       0x080
#define GIC_DIST_IGROUPR1       0x084
#define GIC_DIST_IGROUPRN       0x0FC
#define GIC_DIST_ISENABLER0     0x100
#define GIC_DIST_ISENABLER1     0x104
#define GIC_DIST_ISENABLERN     0x17C
#define GIC_DIST_ICENABLER0     0x180
#define GIC_DIST_ICENABLER1     0x184
#define GIC_DIST_ICENABLERN     0x1fC
#define GIC_DIST_ISPENDR0       0x200
#define GIC_DIST_ISPENDR1       0x204
#define GIC_DIST_ISPENDRN       0x27C
#define GIC_DIST_ICPENDR0       0x280
#define GIC_DIST_ICPENDR1       0x284
#define GIC_DIST_ICPENDRN       0x2FC
#define GIC_DIST_ISACTIVER0     0x300
#define GIC_DIST_ISACTIVER1     0x304
#define GIC_DIST_ISACTIVERN     0x37C
#define GIC_DIST_ICACTIVER0     0x380
#define GIC_DIST_ICACTIVER1     0x384
#define GIC_DIST_ICACTIVERN     0x3FC
#define GIC_DIST_IPRIORITYR0    0x400
#define GIC_DIST_IPRIORITYR7    0x41C
#define GIC_DIST_IPRIORITYR8    0x420
#define GIC_DIST_IPRIORITYRN    0x7F8
#define GIC_DIST_ITARGETSR0     0x800
#define GIC_DIST_ITARGETSR7     0x81C
#define GIC_DIST_ITARGETSR8     0x820
#define GIC_DIST_ITARGETSRN     0xBF8
#define GIC_DIST_ICFGR0         0xC00
#define GIC_DIST_ICFGR1         0xC04
#define GIC_DIST_ICFGR2         0xC08
#define GIC_DIST_ICFGRN         0xCFC
#define GIC_DIST_NSACR0         0xE00
#define GIC_DIST_NSACRN         0xEFC
#define GIC_DIST_SGIR           0xF00
#define GIC_DIST_CPENDSGIR0     0xF10
#define GIC_DIST_CPENDSGIRN     0xF1C
#define GIC_DIST_SPENDSGIR0     0xF20
#define GIC_DIST_SPENDSGIRN     0xF2C

/*
 * ARM Generic Interrupt Controller (Architecture version 2.0)
 * Architecture Specification (Issue B.b)
 * 4.3.15: Software Generated Interrupt Register, GICD_SGI
 * Values defined as per Table 4-21 (GICD_SGIR bit assignments)
 */
#define GIC_DIST_SGI_TARGET_LIST_FILTER_SHIFT   24
#define GIC_DIST_SGI_TARGET_LIST_FILTER_MASK    0x3 << GIC_DIST_SGI_TARGET_LIST_FILTER_SHIFT
#define GIC_DIST_SGI_TARGET_LIST_SPEC           0
#define GIC_DIST_SGI_TARGET_LIST_OTHERS         1
#define GIC_DIST_SGI_TARGET_SELF                2

#define GIC_DIST_SGI_CPU_TARGET_LIST_SHIFT      16
#define GIC_DIST_SGI_CPU_TARGET_LIST_MASK       0xFF << GIC_DIST_SGI_CPU_TARGET_LIST_SHIFT

#define GIC_DIST_SGI_INTID_MASK                 0xF

bool vgic_inject_irq(size_t vcpu_id, int irq);

typedef struct gic_dist_map vgic_reg_t;

static inline struct gic_dist_map *vgic_get_dist(void *registers)
{
    return (struct gic_dist_map *) registers;
}

static inline bool vgic_dist_is_enabled(struct gic_dist_map *gic_dist)
{
    return gic_dist->ctlr == GIC_ENABLED;
}

static inline void vgic_dist_enable(struct gic_dist_map *gic_dist) {
    gic_dist->ctlr = GIC_ENABLED;
}

static inline void vgic_dist_disable(struct gic_dist_map *gic_dist)
{
    gic_dist->ctlr = 0;
}
