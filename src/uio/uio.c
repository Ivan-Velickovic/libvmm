
/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libvmm/arch/aarch64/fault.h>
#include <libvmm/util/util.h>
#include <libvmm/virq.h>
#include <microkit.h>
#include <stddef.h>
#include <stdint.h>

bool uio_notify_fault_handle(size_t vcpu_id, size_t offset, size_t fsr,
                             seL4_UserContext *regs, void *data) {
  microkit_channel ch = *(microkit_channel *)data;
  if (fault_is_read(fsr)) {
    LOG_VMM_ERR(
        "Read into VMM notify region, but uio driver should never do that\n");
    return false;
  } else {
    microkit_notify(ch);
  }
  return true;
}

void uio_ack(size_t vcpu_id, int irq, void *cookie) {
  /* Do nothing for UIO ack */
}

bool uio_register_driver(int irq, microkit_channel *ch,
                         uintptr_t notify_region_base,
                         size_t notify_region_size) {
  bool success = virq_register(GUEST_VCPU_ID, irq, uio_ack, NULL);
  assert(success);
  success = fault_register_vm_exception_handler(
      notify_region_base, notify_region_size, &uio_notify_fault_handle, ch);
  if (!success) {
    LOG_VMM_ERR("Could not register uio virtual memory fault handler for "
                "uio notify region [0x%lx..0x%lx)\n",
                notify_region_base, notify_region_base + notify_region_size);
    return false;
  }
  return true;
}
