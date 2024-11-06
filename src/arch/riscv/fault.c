/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libvmm/tcb.h>
#include <libvmm/vcpu.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/riscv/plic.h>
#include <libvmm/arch/riscv/fault.h>

#define WFI_INST    0x10500073

// TODO: why does our impl id and version id come up as this?
/* [    0.000000] SBI implementation ID=0xfffffffffffffdf4 Version=0xfffffffffffffdf4 */

/* We support version 2.0 of SBI */
#define SBI_SPEC_MAJOR_VERSION 2
#define SBI_SPEC_MINOR_VERSION 0
#define SBI_SPEC_VERSION (SBI_SPEC_MAJOR_VERSION << 24) | (SBI_SPEC_MINOR_VERSION)

#define MACHINE_ARCH_ID 0
#define MACHINE_IMPL_ID 0
#define MACHINE_VENDOR_ID 0

/*
 * List of SBI extensions. Note that what extensions
 * we actually support is a subset of this list.
 */
// TODO: support system suspend
enum sbi_extensions {
    SBI_EXTENSION_BASE = 0x10,
    SBI_EXTENSION_TIMER = 0x54494d45,
    SBI_EXTENSION_HART_STATE_MANAGEMENT = 0x48534d,
    SBI_EXTENSION_SYSTEM_RESET = 0x53525354,
    SBI_EXTENSION_DEBUG_CONSOLE = 0x4442434e,
};

enum sbi_debug_extension {
    SBI_CONSOLE_PUTCHAR = 1,
};

enum sbi_timer_function {
    SBI_TIMER_SET = 0,
};

enum sbi_base_function {
    SBI_BASE_GET_SBI_SPEC_VERSION = 0,
    SBI_BASE_GET_SBI_IMPL_ID = 1,
    SBI_BASE_GET_SBI_IMPL_VERSION = 2,
    SBI_BASE_PROBE_EXTENSION_ID = 3,
    SBI_BASE_GET_MACHINE_VENDOR_ID = 4,
    SBI_BASE_GET_MACHINE_ARCH_ID = 5,
    SBI_BASE_GET_MACHINE_IMPL_ID = 6,
};

enum sbi_return_code {
    SBI_SUCCESS = 0,
    SBI_ERR_FAILED = -1,
    SBI_ERR_NOT_SUPPORTED = -2,
    SBI_ERR_INVALID_PARAM = -3,
    SBI_ERR_DENIED = -4,
    SBI_ERR_INVALID_ADDRESS = -5,
    SBI_ERR_ALREADY_AVAILABLE = -6,
    SBI_ERR_ALREADY_STARTED = -7,
    SBI_ERR_ALREADY_STOPPED = -8,
    SBI_ERR_NO_SHMEM = -9,
};

enum trap_cause {
    TRAP_ILLEGAL_INSTRUCTION = 2,
    TRAP_ENV_CALL_VS_MODE = 10,
    TRAP_VIRTUAL_INSTRUCTION = 22,
};

#define SIP_TIMER (1 << 5)

#define BIT(n) (1ul<<(n))
#define PT_SIZE 512
#define GET_PPN(x)      (x & 0xfffffffffff)
#define VPN_MASK        0x1ff
#define VPN_SHIFT(l)    (12 + 9 * (l))
#define GET_VPN(x, l)   (((x) >> VPN_SHIFT(l)) & VPN_MASK)
#define PPN_SHIFT       12
#define PTE_V           BIT(0)
#define PTE_R           BIT(1)
#define PTE_W           BIT(2)
#define PTE_X           BIT(3)
#define PTE_GET_1G(x)   (((x >> 28) & 0x3ffffff))
#define PTE_GET_2M(x)   (((x >> 19) & (BIT(36) - 1)))
#define PTE_GET_4K(x)   (((x >> 10) & (BIT(45) - 1)))
#define SV39_MODE       0x8

static uint64_t pt[512];

static seL4_Word guest_virtual_physical(seL4_Word addr, size_t vcpu_id) {
    size_t level = 2;
    // uint64_t *ppt = &pt;
    for (int i = 0; i < PT_SIZE; i++) {
        pt[i] = 0;
    }

    seL4_Word satp = 0;
    seL4_RISCV_VCPU_ReadRegs_t res = seL4_RISCV_VCPU_ReadRegs(BASE_VCPU_CAP + vcpu_id, seL4_VCPUReg_SATP);
    assert(!res.error);
    satp = res.value;
    // LOG_VMM("satp: 0x%lx\n", satp);
    uint8_t mode = satp >> 60;
    // LOG_VMM("mode is 0x%lx\n", mode);

    assert(mode == SV39_MODE);
    // switch mode {
    // case SV39_MODE:
    // case 
    // }

    seL4_Word ppn = GET_PPN(satp);
    seL4_Word gpa = ppn << PPN_SHIFT;
    uint64_t pte = 0;

    /* Read the guest's page table */
    while (level > 0) {
        // LOG_VMM("copy in %lx level %d\n", gpa, level);
        assert(gpa != 0);
        for (int i = 0; i < PT_SIZE; i++) {
            pt[i] = *(uint64_t *)(gpa + 8 * i);
        }
        int vpn = GET_VPN(addr, level);
        pte = pt[vpn];
        if (pte & PTE_V && (pte & PTE_R || pte & PTE_X)) {
            /* we reach a leaf page */
            if (level == 2) {
                /* 1 GiB page */
                return (PTE_GET_1G(pte) << 30)  | (addr & (BIT(30) - 1));
            }
            if (level == 1) {
                /* 2 MiB page */
                return (PTE_GET_2M(pte) << 21) | (addr & (BIT(21) - 1));
            }
            if (level == 0) {
                /* 4 KiB page */
                return ((PTE_GET_4K(pte) << 12) | (addr & (BIT(12) - 1)));
            }

        }
        gpa = (pte >> 10) << 12;
        level--;
    }

    assert(false);
    return 0;
}

extern uintptr_t guest_ram_vaddr;

#define FUNCT3_CSW 0b110
#define FUNCT3_CLW 0b010

struct fault_instruction fault_decode_instruction(size_t vcpu_id, seL4_UserContext *regs, seL4_Word ip) {
    // LOG_VMM("decoding at ip 0x%lx\n", ip);
    seL4_Word guest_physical = guest_virtual_physical(ip, vcpu_id);
    assert(guest_physical >= guest_ram_vaddr && guest_physical <= guest_ram_vaddr + 0x10000000);
    // LOG_VMM("guest_physical: 0x%lx\n", guest_physical);
    /*
     * For guest OSes that use the RISC-V 'C' extension we must read the instruction 16-bits at
     * a time, in order to avoid UB since it is valid for *all* instructions, compressed or not,
     * to be aligned to 16-bits.
     */
    uint16_t instruction_lo = *((uint16_t *)guest_physical);
    uint16_t instruction_hi = *(uint16_t *)(guest_physical + 16);
    uint32_t instruction = ((uint32_t)instruction_hi << 16) | instruction_lo;
    // LOG_VMM("guest_physical: 0x%lx, instruction: 0x%lx, instruction_lo: 0x%x, instruction_hi: 0x%x\n", guest_physical, instruction, instruction_lo, instruction_hi);
    // TODO: check this.
    uint8_t op_code = instruction & 0x7f;
    /* funct3 is from bits 12:14. */
    // uint8_t funct3 = (instruction >> 12) & 0x7;

    // uint8_t compressed_funct3 = (funct3 >> 2) | (op_code & 0b11);
    /* Make sure data width is word size. TODO: don't know if we need to have this
     * restriction or how Yanyan found out the data width numbers for funct3. */

    /* If we are in here, we are dealing with a compressed instruction */
    switch (instruction_lo >> 13) {
        case FUNCT3_CSW:
            return (struct fault_instruction){
                .op_code = OP_CODE_STORE,
                .width = 2,
                .rs2 = (instruction_lo >> 2) & (BIT(3) - 1),
            };
        case FUNCT3_CLW:
            return (struct fault_instruction){
                .op_code = OP_CODE_LOAD,
                .width = 2,
                .rd = (instruction_lo >> 2) & (BIT(3) - 1),
            };
        default:
            break;
    }

    switch (op_code) {
    case OP_CODE_STORE:
        return (struct fault_instruction){
            .op_code = OP_CODE_STORE,
            .width = 4,
            .rs2 = (instruction >> 20) & (BIT(5) - 1),
        };
    case OP_CODE_LOAD:
        return (struct fault_instruction){
            .op_code = OP_CODE_LOAD,
            .width = 4,
            .rd = (instruction >> 7) & (BIT(5) - 1),
        };
    default:
        LOG_VMM_ERR("invalid op code 0x%x\n", op_code);
        break;
    }

    // assert(funct3 == 2);

    /* TODO: not sure if there's a better way to do this */
    LOG_VMM_ERR("could not decode instruction");
    assert(false);

    return (struct fault_instruction){ 0 };
}

char *fault_to_string(seL4_Word fault_label) {
    // TODO
    switch (fault_label) {
    case seL4_Fault_VMFault: return "virtual memory exception";
    case seL4_Fault_UnknownSyscall: return "unknown syscall";
    case seL4_Fault_UserException: return "user exception";
    case seL4_Fault_VCPUFault: return "vCPU";
    default: return "unknown fault";
    }
}

static bool fault_handle_sbi_timer(size_t vcpu_id, seL4_Word sbi_fid, seL4_UserContext *regs) {
    switch (sbi_fid) {
    case SBI_TIMER_SET: {
        /* stime_value is always 64-bit */
        uint64_t stime_value = regs->a0;

        seL4_RISCV_VCPU_ReadRegs_t res = seL4_RISCV_VCPU_ReadRegs(BASE_VCPU_CAP + vcpu_id, seL4_VCPUReg_SIP);
        assert(!res.error);
        res.value &= ~SIP_TIMER;
        seL4_RISCV_VCPU_WriteRegs(BASE_VCPU_CAP + vcpu_id, seL4_VCPUReg_SIP, res.value);

        uint64_t curr_time = 0;
        asm volatile("rdtime %0" : "=r"(curr_time));
        if (curr_time >= stime_value) {
            /* In this case we are already past the target time, so we immediately inject an IRQ. */
            // TODO: check return value
            plic_inject_timer_irq(vcpu_id);
        } else {
            seL4_Error err = seL4_RISCV_VCPU_WriteRegs(BASE_VCPU_CAP + vcpu_id, seL4_VCPUReg_TIMER, stime_value);
            assert(!err);
        }
        // LOG_VMM("setting timer stime_value: 0x%lx, curr_time: 0x%lx\n", stime_value, curr_time);

        regs->a0 = SBI_SUCCESS;

        return true;
    }
    default:
        return false;
    }
}

static bool fault_handle_sbi_base(size_t vcpu_id, seL4_Word sbi_fid, seL4_UserContext *regs) {
    switch (sbi_fid) {
    case SBI_BASE_GET_SBI_SPEC_VERSION:
        regs->a0 = SBI_SUCCESS;
        regs->a1 = SBI_SPEC_VERSION;
        return true;
    case SBI_BASE_GET_SBI_IMPL_ID:
    case SBI_BASE_GET_SBI_IMPL_VERSION:
        /* We do not emulate a specific SBI implementation. */
        regs->a0 = SBI_ERR_NOT_SUPPORTED;
        return true;
    case SBI_BASE_GET_MACHINE_VENDOR_ID:
        regs->a0 = SBI_SUCCESS;
        regs->a1 = MACHINE_VENDOR_ID;
        return true;
    case SBI_BASE_GET_MACHINE_ARCH_ID:
        regs->a0 = SBI_SUCCESS;
        regs->a1 = MACHINE_ARCH_ID;
        return true;
    case SBI_BASE_GET_MACHINE_IMPL_ID:
        regs->a0 = SBI_SUCCESS;
        regs->a1 = MACHINE_IMPL_ID;
        return true;
    case SBI_BASE_PROBE_EXTENSION_ID: {
        seL4_Word probe_eid = regs->a0;
        switch (probe_eid) {
        case SBI_EXTENSION_BASE:
        case SBI_EXTENSION_TIMER:
        case SBI_EXTENSION_HART_STATE_MANAGEMENT:
        case SBI_EXTENSION_SYSTEM_RESET:
        case SBI_EXTENSION_DEBUG_CONSOLE:
            regs->a0 = SBI_SUCCESS;
            regs->a1 = 1;
            return true;
        default:
        // TODO: print out string name of extension
            LOG_VMM("guest probed for SBI EID 0x%lx that is not supported\n", probe_eid);
            regs->a0 = SBI_ERR_NOT_SUPPORTED;
            return true;
        }
    }
    default:
        LOG_VMM_ERR("could not handle SBI base extension call with FID: 0x%lx\n", sbi_fid);
        break;
    }

    return false;
}

static bool fault_handle_sbi(size_t vcpu_id, seL4_UserContext *regs) {
    /* SBI extension ID */
    seL4_Word sbi_eid = regs->a7;
    /* SBI function ID for the given extension */
    seL4_Word sbi_fid = regs->a6;
    // LOG_VMM("SBI handle EID 0x%lx, FID: 0x%lx\n", sbi_eid, sbi_fid);
    switch (sbi_eid) {
    case SBI_EXTENSION_BASE:
        // TODO: error handling
        fault_handle_sbi_base(vcpu_id, sbi_fid, regs);
        regs->pc += 4;
        seL4_TCB_WriteRegisters(BASE_VM_TCB_CAP + vcpu_id, false, 0, sizeof(seL4_UserContext) / sizeof(seL4_Word), regs);
        return true;
    case SBI_EXTENSION_TIMER:
        fault_handle_sbi_timer(vcpu_id, sbi_fid, regs);
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
        LOG_VMM_ERR("unhandled sbi_eid: 0x%lx, sbi_fid 0x%lx\n", sbi_eid, sbi_fid);
    }
    }
    return false;
}

bool fault_handle(size_t vcpu_id, microkit_msginfo msginfo) {
    seL4_UserContext regs;
    seL4_TCB_ReadRegisters(BASE_VM_TCB_CAP + vcpu_id, false, 0, sizeof(seL4_UserContext) / sizeof(seL4_Word), &regs);
    size_t label = microkit_msginfo_get_label(msginfo);
    // LOG_VMM("handling fault '%s'\n", fault_to_string(label));
    bool success = false;
    switch (label) {
        case seL4_Fault_VMFault: {
            // LOG_VMM("fault on addr 0x%lx at pc 0x%lx\n", seL4_GetMR(seL4_VMFault_Addr), regs.pc);
            seL4_Word addr = seL4_GetMR(seL4_VMFault_Addr);
            seL4_Word fsr = seL4_GetMR(seL4_VMFault_FSR);
            if (addr >= PLIC_ADDR && addr < PLIC_ADDR + PLIC_SIZE) {
                assert(seL4_GetMR(seL4_VMFault_IP) == regs.pc);
                return plic_handle_fault(vcpu_id, addr - PLIC_ADDR, fsr, &regs);
            }
            return false;
        }
        case seL4_Fault_VCPUFault: {
            seL4_Word cause = seL4_GetMR(seL4_VCPUFault_Cause);
            if (cause == TRAP_ENV_CALL_VS_MODE) {
                return fault_handle_sbi(vcpu_id, &regs);
            } else if (cause == TRAP_VIRTUAL_INSTRUCTION) {
                uint32_t data = seL4_GetMR(seL4_VCPUFault_Data);
                // if (data == WFI_INST) {
                    // TODO: handle WFI properly
                    // regs.pc += 4;
                    // seL4_TCB_WriteRegisters(BASE_VM_TCB_CAP + vcpu_id, false, 0, sizeof(seL4_UserContext) / sizeof(seL4_Word), &regs);
                    // return true;
                // } else {
                LOG_VMM_ERR("unknown vCPU virtual instruction fault, data: 0x%lx, pc: 0x%lx\n", data, regs.pc);
                return false;
                // }
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
