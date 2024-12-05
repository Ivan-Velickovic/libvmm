/*
 * Copyright 2023, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdbool.h>
#include <microkit.h>
#include <libvmm/tcb.h>
#include <libvmm/util/util.h>

void tcb_print_regs(size_t vcpu_id) {
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
    printf("    pc: 0x%016lx\n", regs.pc);
    printf("    ra: 0x%016lx\n", regs.ra);
    printf("    gp: 0x%016lx\n", regs.gp);
    printf("    sp: 0x%016lx\n", regs.sp);

    printf("    s0: 0x%016lx\n", regs.s0);
    printf("    s1: 0x%016lx\n", regs.s1);
    printf("    s2: 0x%016lx\n", regs.s2);
    printf("    s3: 0x%016lx\n", regs.s3);
    printf("    s4: 0x%016lx\n", regs.s4);
    printf("    s5: 0x%016lx\n", regs.s5);
    printf("    s6: 0x%016lx\n", regs.s6);
    printf("    s7: 0x%016lx\n", regs.s7);
    printf("    s8: 0x%016lx\n", regs.s8);
    printf("    s9: 0x%016lx\n", regs.s9);
    printf("   s10: 0x%016lx\n", regs.s10);
    printf("   s11: 0x%016lx\n", regs.s11);

    printf("    a0: 0x%016lx\n", regs.a0);
    printf("    a1: 0x%016lx\n", regs.a1);
    printf("    a2: 0x%016lx\n", regs.a2);
    printf("    a3: 0x%016lx\n", regs.a3);
    printf("    a4: 0x%016lx\n", regs.a4);
    printf("    a5: 0x%016lx\n", regs.a5);
    printf("    a6: 0x%016lx\n", regs.a6);
    printf("    a7: 0x%016lx\n", regs.a7);

    printf("    t0: 0x%016lx\n", regs.t0);
    printf("    t1: 0x%016lx\n", regs.t1);
    printf("    t2: 0x%016lx\n", regs.t2);
    printf("    t3: 0x%016lx\n", regs.t3);
    printf("    t4: 0x%016lx\n", regs.t4);
    printf("    t5: 0x%016lx\n", regs.t5);
    printf("    t6: 0x%016lx\n", regs.t6);

    printf("    tp: 0x%016lx\n", regs.tp);
}
