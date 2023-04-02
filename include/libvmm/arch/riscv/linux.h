#pragma once

#include <stdbool.h>
#include <stdint.h>

// The structure of the kernel image header and the magic value comes from the
// Linux kernel documentation that can be found here:
// https://www.kernel.org/doc/Documentation/riscv/boot-image-header.txt

#define LINUX_IMAGE_MAGIC   0x5643534952
#define LINUX_IMAGE_MAGIC_2 0x56534905

struct linux_image_header {
    uint32_t code0;       // Executable code
    uint32_t code1;       // Executable code
    uint64_t text_offset; // Image load offset, little endian
    uint64_t image_size;  // Effective Image size, little endian
    uint64_t flags;       // kernel flags, little endian
    uint32_t version;     // Version of this header
    uint32_t res1;        // Reserved
    uint64_t res2;        // Reserved
    uint64_t magic;       // Magic number, little endian, "RISCV"
    uint32_t magic2;      // Magic number 2, little endian, "RSC\x05"
    uint32_t res4;        // Reserved for PE COFF offset
};

uintptr_t linux_setup_images(uintptr_t ram_start,
                             uintptr_t kernel,
                             size_t kernel_size,
                             uintptr_t dtb_src,
                             uintptr_t dtb_dest,
                             size_t dtb_size,
                             uintptr_t initrd_src,
                             uintptr_t initrd_dest,
                             size_t initrd_size);

