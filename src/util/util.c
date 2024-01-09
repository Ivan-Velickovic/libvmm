/*
 * Copyright 2022, UNSW (ABN 57 195 873 179)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "util.h"

/* This is required to use the printf library we brought in, it is
   simply for convenience since there's a lot of logging/debug printing
   in the VMM. */
void _putchar(char character)
{
    microkit_dbg_putc(character);
}

void print_bitarray(bitarray_t* bitarr)
{
    for (int i = 0; i < bitarr->num_of_words; i++)
    {
        printf("%d:", i);
        for (int j = 0; j < sizeof(bitarr->words[i]) * 8; j++)
        {
            printf("%lu", (bitarr->words[i] >> j) & 1);
        }
        printf("\n");
    }
}

void print_binary(word_t word) {
    for (int i = 63; i >= 0; i--) {
        printf("%llu", (word >> i) & 1);

        if (i % 8 == 0 && i != 0) {
            printf(" ");
        }
    }
    printf("\n");
}
