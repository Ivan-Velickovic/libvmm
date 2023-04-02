/*
 * Copyright 2017, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libvmm/tcb.h>
#include <libvmm/vcpu.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/aarch64/fault.h>

char *fault_to_string(seL4_Word fault_label) {
    // @riscv
    switch (fault_label) {
        case seL4_Fault_VMFault: return "virtual memory";
        case seL4_Fault_UnknownSyscall: return "unknown syscall";
        case seL4_Fault_UserException: return "user exception";
        case seL4_Fault_VCPUFault: return "VCPU";
        default: return "unknown fault";
    }
}

bool fault_handle(size_t vcpu_id, microkit_msginfo msginfo) {
    size_t label = microkit_msginfo_get_label(msginfo);
    bool success = false;
    switch (label) {
        default:
            /* We have reached a genuinely unexpected case, stop the guest. */
            LOG_VMM_ERR("unknown fault label 0x%lx, stopping guest with ID 0x%lx\n", label, vcpu_id);
            microkit_vcpu_stop(vcpu_id);
            /* Dump the TCB and vCPU registers to hopefully get information as
             * to what has gone wrong. */
            tcb_print_regs(vcpu_id);
            vcpu_print_regs(vcpu_id);
    }

    if (!success) {
        LOG_VMM_ERR("Failed to handle %s fault\n", fault_to_string(label));
    }

    return success;
}
