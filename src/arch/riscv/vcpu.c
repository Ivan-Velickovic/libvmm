/*
 * Copyright 2023, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <microkit.h>
#include <libvmm/vcpu.h>
#include <libvmm/util/util.h>

void vcpu_reset(size_t vcpu_id) {
    // @riscv
}

void vcpu_print_regs(size_t vcpu_id) {
    LOG_VMM("dumping VCPU (ID 0x%lx) registers:\n", vcpu_id);
    printf("    sstatus: 0x%016lx\n", microkit_vcpu_riscv_read_reg(vcpu_id, seL4_VCPUReg_SSTATUS));
    printf("    sie: 0x%016lx\n", microkit_vcpu_riscv_read_reg(vcpu_id, seL4_VCPUReg_SIE));
    printf("    stvec: 0x%016lx\n", microkit_vcpu_riscv_read_reg(vcpu_id, seL4_VCPUReg_STVEC));
    printf("    sepc: 0x%016lx\n", microkit_vcpu_riscv_read_reg(vcpu_id, seL4_VCPUReg_SEPC));
    printf("    scause: 0x%016lx\n", microkit_vcpu_riscv_read_reg(vcpu_id, seL4_VCPUReg_SCAUSE));
    printf("    stval: 0x%016lx\n", microkit_vcpu_riscv_read_reg(vcpu_id, seL4_VCPUReg_STVAL));
    printf("    sip: 0x%016lx\n", microkit_vcpu_riscv_read_reg(vcpu_id, seL4_VCPUReg_SIP));
    printf("    timer: 0x%016lx\n", microkit_vcpu_riscv_read_reg(vcpu_id, seL4_VCPUReg_TIMER));
}
