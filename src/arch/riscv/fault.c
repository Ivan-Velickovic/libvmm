/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libvmm/tcb.h>
#include <libvmm/vcpu.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/aarch64/fault.h>

#define PLIC_ADDR 0xc000000
#define PLIC_SIZE 0x4000000

#define OP_CODE_LOAD 0b0000011
#define OP_CODE_STORE 0b0100011

#define SBI_SPEC_MAJOR_VERSION 2
#define SBI_SPEC_MINOR_VERSION 0

#define SBI_SPEC_VERSION (SBI_SPEC_MAJOR_VERSION << 24) | (SBI_SPEC_MINOR_VERSION)

enum sbi_call {
    SBI_CONSOLE_PUTCHAR = 1,
};

enum sbi_base_function {
    SBI_BASE_GET_SBI_SPEC_VERSION = 0,
    SBI_BASE_PROBE_EXTENSION_ID = 3,
    SBI_BASE_GET_MACHINE_VENDOR_ID = 4,
    SBI_BASE_GET_MACHINE_ARCH_ID = 5,
    SBI_BASE_GET_MACHINE_IMPL_ID = 6,
};

enum trap_cause {
    TRAP_ILLEGAL_INSTRUCTION = 2,
    TRAP_ENV_CALL_VS_MODE = 10,
};

void fault_decode_instruction(seL4_UserContext *regs, seL4_Word ip) {
    uint32_t instruction = *(uint32_t *)ip;
    // TODO: check this.
    uint32_t op_code = instruction & 0x7f;
    switch (op_code) {
    case OP_CODE_STORE:
    case OP_CODE_LOAD:
        break;
    default:
        LOG_VMM_ERR("invalid op code 0x%x\n", op_code);
        break;
    }
}

bool fault_handle_plic(size_t offset, seL4_Word fsr, seL4_UserContext *regs) {
    fault_decode_instruction(regs, seL4_GetMR(seL4_UserException_FaultIP));
    return false;
}

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

static bool fault_handle_sbi_base(seL4_Word sbi_fid, seL4_UserContext *regs) {
    switch (sbi_fid) {
    case SBI_BASE_GET_SBI_SPEC_VERSION:
        regs->a0 = 0;
        regs->a1 = SBI_SPEC_VERSION;
        return true;
    case SBI_BASE_GET_MACHINE_VENDOR_ID:
    case SBI_BASE_GET_MACHINE_ARCH_ID:
    case SBI_BASE_GET_MACHINE_IMPL_ID:
        regs->a0 = 0;
        regs->a1 = 0;
        return true;
    case SBI_BASE_PROBE_EXTENSION_ID:
        LOG_VMM("a0: 0x%x, a1: 0x%x\n", regs->a0, regs->a1);
        regs->a0 = 0;
        regs->a1 = 0;
        return false;
    default:
        LOG_VMM_ERR("could not handle SBI base extension call with FID: %d\n", sbi_fid);
        break;
    }

    return false;
}

static bool fault_handle_sbi(size_t vcpu_id, seL4_UserContext *regs) {
    /* SBI extension ID */
    seL4_Word sbi_eid = regs->a7;
    /* SBI function ID for the given extension */
    seL4_Word sbi_fid = regs->a6;
    LOG_VMM("SBI handle EID 0x%lx, FID: 0x%lx\n", sbi_eid, sbi_fid);
    switch (sbi_eid) {
    case 0x10:
        fault_handle_sbi_base(sbi_fid, regs);
        regs->pc += 4;
        seL4_TCB_WriteRegisters(BASE_VM_TCB_CAP + vcpu_id, false, 0, sizeof(seL4_UserContext) / sizeof(seL4_Word), regs);
        return true;
    // case SBI_CONSOLE_PUTCHAR: {
    //     printf("%c", regs->a0);
    //     regs->a0 = 0;
    //     regs->pc += 4;
    //     seL4_TCB_WriteRegisters(BASE_VM_TCB_CAP + vcpu_id, false, 0, sizeof(seL4_UserContext) / sizeof(seL4_Word), regs);
    //     return true;
    // }
    default: {
        printf("sbi_eid: %d, sbi_fid %d\n", sbi_eid, sbi_fid);
    }
    }
    return false;
}

bool fault_handle(size_t vcpu_id, microkit_msginfo msginfo) {
    seL4_UserContext regs;
    seL4_TCB_ReadRegisters(BASE_VM_TCB_CAP + vcpu_id, false, 0, sizeof(seL4_UserContext) / sizeof(seL4_Word), &regs);
    size_t label = microkit_msginfo_get_label(msginfo);
    bool success = false;
    switch (label) {
        case seL4_Fault_VMFault: {
            LOG_VMM("fault on addr 0x%lx\n", seL4_GetMR(seL4_VMFault_Addr));
            seL4_Word addr = seL4_GetMR(seL4_VMFault_Addr);
            seL4_Word fsr = seL4_GetMR(seL4_VMFault_FSR);
            if (addr >= PLIC_ADDR && addr < PLIC_ADDR + PLIC_SIZE) {
                return fault_handle_plic(addr - PLIC_ADDR, fsr, &regs);
            }
            return false;
        }
        case seL4_Fault_VCPUFault: {
            seL4_Word cause = seL4_GetMR(seL4_VCPUFault_Cause);
            if (cause == TRAP_ENV_CALL_VS_MODE) {
                return fault_handle_sbi(vcpu_id, &regs);
            } else {
                LOG_VMM_ERR("unhandled vCPU fault cause: 0x%lx\n", seL4_GetMR(seL4_VCPUFault_Cause));
                return false;
            }
        }
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
