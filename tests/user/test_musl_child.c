/*
 * test_musl_child.c — musl-linked child for test_musl harness
 *
 * This binary is linked against musl libc (not bare-metal syscall stubs).
 * It exercises libc functions and exits cleanly.  The parent harness
 * (test_musl.c) runs this via vfork+execve with argv[1] selecting the
 * sub-test, then checks the exit code.
 *
 * This is the primary tool for investigating the double-free on process
 * exit that affects musl-linked binaries on RISC-V.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Sub-test: just exit cleanly (baseline) */
static int test_exit(void)
{
    return 0;
}

/* Sub-test: printf exercises stdio + write syscall */
static int test_printf(void)
{
    int n = printf("test_musl_child: hello from musl printf\n");
    return n > 0 ? 0 : 1;
}

/* Sub-test: malloc + free exercises heap */
static int test_malloc(void)
{
    void *p = malloc(128);
    if (!p) return 1;
    memset(p, 0xAA, 128);
    free(p);

    /* Second allocation to check heap is still functional */
    void *q = malloc(64);
    if (!q) return 1;
    free(q);
    return 0;
}

/* Sub-test: string functions */
static int test_string(void)
{
    const char *s = "hello, musl";
    if (strlen(s) != 11) return 1;

    char buf[32];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, s, 11);
    if (strcmp(buf, s) != 0) return 1;

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_musl_child <subtest>\n");
        return 1;
    }

    if (streq(argv[1], "exit"))    return test_exit();
    if (streq(argv[1], "printf"))  return test_printf();
    if (streq(argv[1], "malloc"))  return test_malloc();
    if (streq(argv[1], "string"))  return test_string();

    fprintf(stderr, "test_musl_child: unknown subtest '%s'\n", argv[1]);
    return 1;
}
