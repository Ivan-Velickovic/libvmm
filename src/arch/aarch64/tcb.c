/*
 * Copyright 2023, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <microkit.h>
#include <stdbool.h>
#include <libvmm/tcb.h>
#include <libvmm/util/util.h>

void tcb_print_regs(size_t vcpu_id)
{
    /*
     * While we are potentially doing an extra system call in order to read the
     * TCB registers (as the VMM may have already read the TCB registers before
     * calling this function), this function should only be used in the context
     * of errors or debug logging and hence the cost of the system call should
     * not be an issue.
     */
    seL4_UserContext regs;
    seL4_Error err = seL4_TCB_ReadRegisters(BASE_VM_TCB_CAP + vcpu_id, false, 0, SEL4_USER_CONTEXT_SIZE, &regs);
    assert(err == seL4_NoError);
    if (err != seL4_NoError) {
        LOG_VMM_ERR("Could not read TCB registers when trying to print TCB registers\n");
        return;
    }
    /* Now dump the TCB registers. */
    LOG_VMM("dumping TCB (ID 0x%lx) registers:\n", vcpu_id);
    /* Frame registers */
    printf("    pc: 0x%016lx\n", regs.pc);
    printf("    sp: 0x%016lx\n", regs.sp);
    printf("    spsr: 0x%016lx\n", regs.spsr);
    printf("    x0: 0x%016lx\n", regs.x0);
    printf("    x1: 0x%016lx\n", regs.x1);
    printf("    x2: 0x%016lx\n", regs.x2);
    printf("    x3: 0x%016lx\n", regs.x3);
    printf("    x4: 0x%016lx\n", regs.x4);
    printf("    x5: 0x%016lx\n", regs.x5);
    printf("    x6: 0x%016lx\n", regs.x6);
    printf("    x7: 0x%016lx\n", regs.x7);
    printf("    x8: 0x%016lx\n", regs.x8);
    printf("    x16: 0x%016lx\n", regs.x16);
    printf("    x17: 0x%016lx\n", regs.x17);
    printf("    x18: 0x%016lx\n", regs.x18);
    printf("    x29: 0x%016lx\n", regs.x29);
    printf("    x30: 0x%016lx\n", regs.x30);
    /* Other integer registers */
    printf("    x9: 0x%016lx\n", regs.x9);
    printf("    x10: 0x%016lx\n", regs.x10);
    printf("    x11: 0x%016lx\n", regs.x11);
    printf("    x12: 0x%016lx\n", regs.x12);
    printf("    x13: 0x%016lx\n", regs.x13);
    printf("    x14: 0x%016lx\n", regs.x14);
    printf("    x15: 0x%016lx\n", regs.x15);
    printf("    x19: 0x%016lx\n", regs.x19);
    printf("    x20: 0x%016lx\n", regs.x20);
    printf("    x21: 0x%016lx\n", regs.x21);
    printf("    x22: 0x%016lx\n", regs.x22);
    printf("    x23: 0x%016lx\n", regs.x23);
    printf("    x24: 0x%016lx\n", regs.x24);
    printf("    x25: 0x%016lx\n", regs.x25);
    printf("    x26: 0x%016lx\n", regs.x26);
    printf("    x27: 0x%016lx\n", regs.x27);
    printf("    x28: 0x%016lx\n", regs.x28);
    /* Thread ID registers */
    printf("    tpidr_el0: 0x%016lx\n", regs.tpidr_el0);
    printf("    tpidrro_el0: 0x%016lx\n", regs.tpidrro_el0);
}
