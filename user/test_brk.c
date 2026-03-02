/*
 * test_brk.c — heap growth via brk
 */

#include "utest.h"

int main(void)
{
    /* 1. Query current break */
    uint32_t brk0 = (uint32_t)(uintptr_t)brk((void *)0);
    UT_ASSERT(brk0 > 0, "initial brk should be non-zero");

    /* 2. Grow heap by 256 bytes */
    uint32_t brk1 = (uint32_t)(uintptr_t)brk((void *)(uintptr_t)(brk0 + 256));
    UT_ASSERT_EQ(brk1, brk0 + 256);

    /* 3. Write to newly allocated heap */
    volatile uint8_t *heap = (volatile uint8_t *)(uintptr_t)brk0;
    heap[0] = 0xAA;
    heap[255] = 0x55;
    UT_ASSERT_EQ(heap[0], 0xAA);
    UT_ASSERT_EQ(heap[255], 0x55);

    /* 4. Shrink heap back */
    uint32_t brk2 = (uint32_t)(uintptr_t)brk((void *)(uintptr_t)brk0);
    UT_ASSERT_EQ(brk2, brk0);

    UT_SUMMARY("test_brk");
}
