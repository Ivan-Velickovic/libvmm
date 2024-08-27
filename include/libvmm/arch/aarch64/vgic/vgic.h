/*
 * Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <microkit.h>
#include <stdbool.h>
#include <stdint.h>
#include <libvmm/virq.h>

#if defined(CONFIG_PLAT_QEMU_ARM_VIRT)
#define GIC_V2
#define GIC_DIST_PADDR      0x8000000
#elif defined(CONFIG_PLAT_ODROIDC4)
#define GIC_V2
#define GIC_DIST_PADDR      0xffc01000
#elif defined(CONFIG_PLAT_MAAXBOARD)
#define GIC_V3
#define GIC_DIST_PADDR      0x38800000
#define GIC_REDIST_PADDR    0x38880000
#elif defined(CONFIG_PLAT_ZYNQMP)
#define GIC_V2
#define GIC_DIST_PADDR      0xf9010000
#else
#error Need to define GIC addresses
#endif

#if defined(GIC_V2)
#define GIC_DIST_SIZE 0x1000
#elif defined(GIC_V3)
#define GIC_DIST_SIZE       0x10000
#define GIC_REDIST_SIZE     0xc0000
#else
#error Unknown GIC version
#endif

/* Uncomment these defines for more verbose logging in the GIC driver. */
// #define DEBUG_IRQ
// #define DEBUG_DIST

#if defined(DEBUG_IRQ)
#define LOG_IRQ(...) do{ printf("VGIC|IRQ: "); printf(__VA_ARGS__); }while(0)
#else
#define LOG_IRQ(...) do{}while(0)
#endif

#if defined(DEBUG_DIST)
#define LOG_DIST(...) do{ printf("VGIC|DIST: "); printf(__VA_ARGS__); }while(0)
#else
#define LOG_DIST(...) do{}while(0)
#endif

void vgic_init();

bool vgic_handle_fault_maintenance(size_t vcpu_id);

bool vgic_handle_fault_dist(size_t vcpu_id, size_t offset, size_t fsr, seL4_UserContext *regs, void *data);
bool vgic_handle_fault_redist(size_t vcpu_id, size_t offset, size_t fsr, seL4_UserContext *regs, void *data);

bool vgic_register_irq(size_t vcpu_id, int virq_num, virq_ack_fn_t ack_fn, void *ack_data);
bool vgic_inject_irq(size_t vcpu_id, int irq);
