/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libvmm/util/util.h>
#include <libvmm/arch/aarch64/smc.h>
#include <libvmm/arch/aarch64/psci.h>

// Values in this file are taken from:
// SMC CALLING CONVENTION
// System Software on ARM (R) Platforms
// Issue B
#define SMC_SERVICE_CALL_MASK 0x3F
#define SMC_SERVICE_CALL_SHIFT 24

#define SMC_FUNC_ID_MASK 0xFFFF

/* SMC and HVC function identifiers */
typedef enum {
    SMC_CALL_ARM_ARCH = 0,
    SMC_CALL_CPU_SERVICE = 1,
    SMC_CALL_SIP_SERVICE = 2,
    SMC_CALL_OEM_SERVICE = 3,
    SMC_CALL_STD_SERVICE = 4,
    SMC_CALL_STD_HYP_SERVICE = 5,
    SMC_CALL_VENDOR_HYP_SERVICE = 6,
    SMC_CALL_TRUSTED_APP = 48,
    SMC_CALL_TRUSTED_OS = 50,
    SMC_CALL_RESERVED = 64,
} smc_call_id_t;

static smc_call_id_t smc_get_call(size_t func_id)
{
    uint64_t service = ((func_id >> SMC_SERVICE_CALL_SHIFT) & SMC_SERVICE_CALL_MASK);
    assert(service >= 0 && service <= 0xFFFF);

    if (service <= SMC_CALL_VENDOR_HYP_SERVICE) {
        return service;
    } else if (service < SMC_CALL_TRUSTED_APP) {
        return SMC_CALL_RESERVED;
    } else if (service < SMC_CALL_TRUSTED_OS) {
        return SMC_CALL_TRUSTED_APP;
    } else if (service < SMC_CALL_RESERVED) {
        return SMC_CALL_TRUSTED_OS;
    } else {
        return SMC_CALL_RESERVED;
    }
}

static inline uint64_t smc_get_function_number(seL4_UserContext *regs)
{
    return regs->x0 & SMC_FUNC_ID_MASK;
}

inline void smc_set_return_value(seL4_UserContext *u, uint64_t val)
{
    u->x0 = val;
}

uint64_t smc_get_arg(seL4_UserContext *u, uint64_t arg)
{
    switch (arg) {
    case 1:
        return u->x1;
    case 2:
        return u->x2;
    case 3:
        return u->x3;
    case 4:
        return u->x4;
    case 5:
        return u->x5;
    case 6:
        return u->x6;
    default:
        LOG_VMM_ERR("trying to get SMC arg: 0x%lx, SMC only has 6 argument registers\n", arg);
        // @ivanv: come back to this
        return 0;
    }
}

static void smc_set_arg(seL4_UserContext *u, size_t arg, size_t val)
{
    switch (arg) {
    case 1:
        u->x1 = val;
        break;
    case 2:
        u->x2 = val;
        break;
    case 3:
        u->x3 = val;
        break;
    case 4:
        u->x4 = val;
        break;
    case 5:
        u->x5 = val;
        break;
    case 6:
        u->x6 = val;
        break;
    default:
        LOG_VMM_ERR("trying to set SMC arg: 0x%lx, with val: 0x%lx, SMC only has 6 argument registers\n", arg, val);
    }
}

// @ivanv: print out which SMC call as a string we can't handle.
bool handle_smc(size_t vcpu_id, uint32_t hsr)
{
    // @ivanv: An optimisation to be made is to store the TCB registers so we don't
    // end up reading them multiple times
    seL4_UserContext regs;
    int err = seL4_TCB_ReadRegisters(BASE_VM_TCB_CAP + vcpu_id, false, 0, SEL4_USER_CONTEXT_SIZE, &regs);
    assert(err == seL4_NoError);

    size_t fn_number = smc_get_function_number(&regs);
    smc_call_id_t service = smc_get_call(regs.x0);

    switch (service) {
    case SMC_CALL_STD_SERVICE:
        if (fn_number < PSCI_MAX) {
            return handle_psci(vcpu_id, &regs, fn_number, hsr);
        }
        LOG_VMM_ERR("Unhandled SMC: standard service call %lu\n", fn_number);
        break;
    default:
        LOG_VMM_ERR("Unhandled SMC: unknown value service: 0x%lx, function number: 0x%lx\n", service, fn_number);
        break;
    }

    return false;
}
