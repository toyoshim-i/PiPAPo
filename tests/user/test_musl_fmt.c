/*
 * test_musl_fmt.c — musl-linked: formatted output (like ps, df)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    /* snprintf formatting (like ps/df output) */
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "PID %5d  MEM %4dK  %s\n",
                     42, 1024, "test_process");
    if (n <= 0) return 1;
    printf("%s", buf);

    /* More format specifiers */
    n = snprintf(buf, sizeof(buf), "%08x %ld %u %% %c",
                 0xDEADBEEF, -42L, 12345U, 'Z');
    if (n <= 0) return 1;

    /* Heavy malloc pattern (like busybox internal allocations) */
    void *ptrs[32];
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 32; i++) {
            ptrs[i] = malloc(16 + i * 8);
            if (!ptrs[i]) return 1;
            memset(ptrs[i], 0x55, 16 + i * 8);
        }
        for (int i = 31; i >= 0; i--)
            free(ptrs[i]);
    }
    /* Large allocation */
    void *big = malloc(4096);
    if (!big) return 1;
    memset(big, 0, 4096);
    free(big);

    printf("fmt+malloc: ok\n");
    return 0;
}
