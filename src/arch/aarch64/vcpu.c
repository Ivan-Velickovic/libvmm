/*
 * Copyright 2023, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <microkit.h>
#include <libvmm/vcpu.h>
#include <libvmm/util/util.h>

#define SCTLR_EL1_UCI       (1 << 26)     /* Enable EL0 access to DC CVAU, DC CIVAC, DC CVAC,
                                           and IC IVAU in AArch64 state   */
#define SCTLR_EL1_C         (1 << 2)      /* Enable data and unified caches */
#define SCTLR_EL1_I         (1 << 12)     /* Enable instruction cache       */
#define SCTLR_EL1_CP15BEN   (1 << 5)      /* AArch32 CP15 barrier enable    */
#define SCTLR_EL1_UTC       (1 << 15)     /* Enable EL0 access to CTR_EL0   */
#define SCTLR_EL1_NTWI      (1 << 16)     /* WFI executed as normal         */
#define SCTLR_EL1_NTWE      (1 << 18)     /* WFE executed as normal         */

/* Disable MMU, SP alignment check, and alignment check */
/* A57 default value */
#define SCTLR_EL1_RES      0x30d00800   /* Reserved value */
#define SCTLR_EL1          ( SCTLR_EL1_RES | SCTLR_EL1_CP15BEN | SCTLR_EL1_UTC \
                           | SCTLR_EL1_NTWI | SCTLR_EL1_NTWE )
#define SCTLR_EL1_NATIVE   (SCTLR_EL1 | SCTLR_EL1_C | SCTLR_EL1_I | SCTLR_EL1_UCI)
#define SCTLR_DEFAULT      SCTLR_EL1_NATIVE

bool vcpu_on_state[GUEST_NUM_VCPUS];

bool vcpu_is_on(size_t vcpu_id)
{
    assert(vcpu_id < GUEST_NUM_VCPUS);
    if (vcpu_id >= GUEST_NUM_VCPUS) {
        return false;
    }

    return vcpu_on_state[vcpu_id];
}

void vcpu_set_on(size_t vcpu_id, bool on)
{
    assert(vcpu_id < GUEST_NUM_VCPUS);
    if (vcpu_id >= GUEST_NUM_VCPUS) {
        return;
    }

    vcpu_on_state[vcpu_id] = on;
}

void vcpu_reset(size_t vcpu_id)
{
    // @ivanv this is an incredible amount of system calls
    // Reset registers
    // @ivanv: double check, shouldn't we be setting sctlr?
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_SCTLR, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_TTBR0, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_TTBR1, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_TCR, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_MAIR, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_AMAIR, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_CIDR, 0);
    /* other system registers EL1 */
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_ACTLR, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_CPACR, 0);
    /* exception handling registers EL1 */
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_AFSR0, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_AFSR1, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_ESR, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_FAR, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_ISR, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_VBAR, 0);
    /* thread pointer/ID registers EL0/EL1 */
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_TPIDR_EL1, 0);
#if CONFIG_MAX_NUM_NODES > 1
    /* Virtualisation Multiprocessor ID Register */
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_VMPIDR_EL2, 0);
#endif /* CONFIG_MAX_NUM_NODES > 1 */
    /* general registers x0 to x30 have been saved by traps.S */
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_SP_EL1, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_ELR_EL1, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_SPSR_EL1, 0); // 32-bit
    /* generic timer registers, to be completed */
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_CNTV_CTL, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_CNTV_CVAL, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_CNTVOFF, 0);
    microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_CNTKCTL_EL1, 0);
}

void vcpu_print_regs(size_t vcpu_id)
{
    // @ivanv this is an incredible amount of system calls
    LOG_VMM("dumping VCPU (ID 0x%lx) registers:\n", vcpu_id);
    /* VM control registers EL1 */
    printf("    sctlr: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_SCTLR));
    printf("    ttbr0: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_TTBR0));
    printf("    ttbr1: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_TTBR1));
    printf("    tcr: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_TCR));
    printf("    mair: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_MAIR));
    printf("    amair: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_AMAIR));
    printf("    cidr: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_CIDR));
    /* other system registers EL1 */
    printf("    actlr: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_ACTLR));
    printf("    cpacr: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_CPACR));
    /* exception handling registers EL1 */
    printf("    afsr0: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_AFSR0));
    printf("    afsr1: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_AFSR1));
    printf("    esr: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_ESR));
    printf("    far: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_FAR));
    printf("    isr: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_ISR));
    printf("    vbar: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_VBAR));
    /* thread pointer/ID registers EL0/EL1 */
    printf("    tpidr_el1: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_TPIDR_EL1));
    // @ivanv: I think thins might not be the correct ifdef
#if CONFIG_MAX_NUM_NODES > 1
    /* Virtualisation Multiprocessor ID Register */
    printf("    vmpidr_el2: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_VMPIDR_EL2));
#endif
    /* general registers x0 to x30 have been saved by traps.S */
    printf("    sp_el1: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_SP_EL1));
    printf("    elr_el1: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_ELR_EL1));
    printf("    spsr_el1: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_SPSR_EL1)); // 32-bit // @ivanv what
    /* generic timer registers, to be completed */
    printf("    cntv_ctl: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_CNTV_CTL));
    printf("    cntv_cval: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_CNTV_CVAL));
    printf("    cntvoff: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_CNTVOFF));
    printf("    cntkctl_el1: 0x%016lx\n", microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_CNTKCTL_EL1));
}
