/*
 * Copyright 2023, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libvmm/util/util.h>
#include <libvmm/arch/riscv/linux.h>

static size_t linux_image_header_major_version(struct linux_image_header *h) {
    // The upper 15 bits represent the minor version.
    return (h->version >> 16);
}

static size_t linux_image_header_minor_version(struct linux_image_header *h) {
    // The lower 16 bits represent the minor version.
    return (h->version & ((1 << 16) - 1));
}

static bool check_magic(struct linux_image_header *h) {
    // Unfortunately even the Linux kernel developers could not make reading a
    // magic number a simple operation. The "magic" field is deprecated as of
    // version 0.2 of the image header, all other versions (at the time of
    // writing) use the "magic2" field.
    if (linux_image_header_major_version(h) == 0 &&
            linux_image_header_minor_version(h) == 2) {
        return h->magic == LINUX_IMAGE_MAGIC;
    } else {
        return h->magic2 == LINUX_IMAGE_MAGIC_2;
    }
}

uintptr_t linux_setup_images(uintptr_t ram_start,
                             uintptr_t kernel,
                             size_t kernel_size,
                             uintptr_t dtb_src,
                             uintptr_t dtb_dest,
                             size_t dtb_size,
                             uintptr_t initrd_src,
                             uintptr_t initrd_dest,
                             size_t initrd_size)
{
    // First we inspect the kernel image header to confirm it is a valid image
    // and to determine where in memory to place the image.
    struct linux_image_header *image_header = (struct linux_image_header *) kernel;
    assert(check_magic(image_header));
    if (!check_magic(image_header)) {
        LOG_VMM_ERR("Linux kernel image magic check failed\n");
        return 0;
    }
    // Copy the guest kernel image into the right location
    uintptr_t kernel_dest = ram_start + image_header->text_offset;
    LOG_VMM("Copying guest kernel image to 0x%x (0x%x bytes)\n", kernel_dest, kernel_size);
    memcpy((char *)kernel_dest, (char *)kernel, kernel_size);
    // Copy the guest device tree blob into the right location
    LOG_VMM("Copying guest DTB to 0x%x (0x%x bytes)\n", dtb_dest, dtb_size);
    memcpy((char *)dtb_dest, (char *)dtb_src, dtb_size);
    // Copy the initial RAM disk into the right location
    // @ivanv: add checks for initrd according to Linux docs
    LOG_VMM("Copying guest initial RAM disk to 0x%x (0x%x bytes)\n", initrd_dest, initrd_size);
    memcpy((char *)initrd_dest, (char *)initrd_src, initrd_size);

    return kernel_dest;
}
