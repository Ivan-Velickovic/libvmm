/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdbool.h>
#include <libvmm/guest.h>
#include <libvmm/vcpu.h>
#include <libvmm/util/util.h>
#include <libvmm/arch/aarch64/psci.h>
#include <libvmm/arch/aarch64/smc.h>
#include <libvmm/arch/aarch64/fault.h>

/*
 * The PSCI version is represented by a 32-bit unsigned integer.
 * The upper 15 bits represent the major version.
 * The lower 16 bits represent the minor version.
*/
#define PSCI_MAJOR_VERSION(v) ((v) << 16)
#define PSCI_MINOR_VERSION(v) ((v) & ((1 << 16) - 1))

/* PSCI return codes */
#define PSCI_SUCCESS 0
#define PSCI_NOT_SUPPORTED -1
#define PSCI_INVALID_PARAMETERS -2
#define PSCI_DENIED -3
#define PSCI_ALREADY_ON -4
#define PSCI_ON_PENDING -5
#define PSCI_INTERNAL_FAILURE -6
#define PSCI_NOT_PRESENT -7
#define PSCI_DISABLED -8
#define PSCI_INVALID_ADDRESS -9

bool handle_psci(size_t vcpu_id, seL4_UserContext *regs, uint64_t fn_number, uint32_t hsr)
{
    // @ivanv: write a note about what convention we assume, should we be checking
    // the convention?
    switch (fn_number) {
    case PSCI_VERSION: {
        /* We support PSCI version 1.2 */
        uint32_t version = PSCI_MAJOR_VERSION(1) | PSCI_MINOR_VERSION(2);
        smc_set_return_value(regs, version);
        break;
    }
    case PSCI_CPU_ON: {
        size_t target_vcpu = smc_get_arg(regs, 1);
        if (target_vcpu < GUEST_NUM_VCPUS && target_vcpu >= 0) {
            /* The guest has given a valid target vCPU */
            if (vcpu_is_on(target_vcpu)) {
                smc_set_return_value(regs, PSCI_ALREADY_ON);
            } else {
                /* We have a valid target vCPU, that is not started yet. So let's turn it on. */
                uintptr_t vcpu_entry_point = smc_get_arg(regs, 2);
                size_t context_id = smc_get_arg(regs, 3);

                seL4_UserContext vcpu_regs = {0};
                vcpu_regs.x0 = context_id;
                vcpu_regs.spsr = 5; // PMODE_EL1h
                vcpu_regs.pc = vcpu_entry_point;

                microkit_vcpu_arm_write_reg(target_vcpu, seL4_VCPUReg_VMPIDR_EL2, target_vcpu);

                seL4_Error err = seL4_TCB_WriteRegisters(
                                     BASE_VM_TCB_CAP + target_vcpu,
                                     false, // We'll explcitly start the guest below rather than in this call
                                     0, // No flags
                                     SEL4_USER_CONTEXT_SIZE,
                                     &vcpu_regs
                                 );
                assert(err == seL4_NoError);
                if (err != seL4_NoError) {
                    return err;
                }

                /* Now that we have started the vCPU, we can set is as turned on. */
                vcpu_set_on(target_vcpu, true);

                LOG_VMM("starting guest vCPU (0x%lx) with entry point 0x%lx, context ID: 0x%lx\n", target_vcpu, vcpu_regs.pc,
                        context_id);
                microkit_vcpu_restart(target_vcpu, vcpu_regs.pc);

                smc_set_return_value(regs, PSCI_SUCCESS);
            }
        } else {
            // The guest has requested to turn on a virtual CPU that does
            // not exist.
            smc_set_return_value(regs, PSCI_INVALID_PARAMETERS);
        }
        break;
    }
    case PSCI_MIGRATE_INFO_TYPE:
        /*
         * There are multiple possible return values for MIGRATE_INFO_TYPE.
         * In this case returning 2 will tell the guest that this is a
         * system that does not use a "Trusted OS" as the PSCI
         * specification says.
         */
        smc_set_return_value(regs, 2);
        break;
    case PSCI_FEATURES:
        // @ivanv: seems weird that we just return nothing here.
        smc_set_return_value(regs, PSCI_NOT_SUPPORTED);
        break;
    case PSCI_SYSTEM_RESET: {
        // @refactor come back to
        // bool success = guest_restart();
        // if (!success) {
        //     LOG_VMM_ERR("Failed to restart guest\n");
        //     smc_set_return_value(regs, PSCI_INTERNAL_FAILURE);
        // } else {

        //      * If we've successfully restarted the guest, all we want to do
        //      * is reply to the fault that caused us to handle the PSCI call
        //      * so that the guest can continue executing. We do not need to
        //      * advance the vCPU program counter as we typically do when
        //      * handling a fault since the correct PC has been set when we
        //      * call guest_restart().

        //     return true;
        // }
        break;
    }
    case PSCI_SYSTEM_OFF:
        // @refactor, is it guaranteed that the CPU that does the vCPU request
        // is the boot vcpu?
        guest_stop();
        return true;
    default:
        LOG_VMM_ERR("Unhandled PSCI function ID 0x%lx\n", fn_number);
        return false;
    }

    bool success = fault_advance_vcpu(vcpu_id, regs);
    assert(success);

    return success;
}
