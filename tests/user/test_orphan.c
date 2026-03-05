/*
 * test_orphan.c — Userland test for orphan reparenting to init
 *
 * Verifies that when a parent exits without reaping its child, the orphan
 * child is reparented to init (PID 1) and the process doesn't hang.
 *
 * Test strategy:
 *   1. Read free pages from /proc/meminfo (baseline)
 *   2. vfork child A → child A vforks grandchild B → grandchild B _exit(0)
 *      → child A _exit(0) without waitpid (grandchild B is zombie orphan)
 *   3. Parent reaps child A via waitpid
 *   4. Repeat several times
 *   5. Read free pages — verify user_pages (freed in sys_exit) don't leak.
 *      Stack pages of orphan zombies may still be held until init reaps them.
 *
 * Note: PID 1 (runtests) doesn't have a generic waitpid(-1) loop, so orphan
 * zombies are reparented but not reaped during this test.  The key assertion
 * is that the sequence completes without hanging and user_pages don't leak.
 */

#include "utest.h"

#define O_RDONLY 0

/* Parse "MemFree:     NNN kB" from /proc/meminfo, return NNN */
static int read_free_kb(void)
{
    char buf[256];
    int fd = open("/proc/meminfo", O_RDONLY, 0);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;

    /* Find "MemFree:" */
    int i;
    for (i = 0; i < n - 8; i++) {
        if (buf[i]   == 'M' && buf[i+1] == 'e' && buf[i+2] == 'm' &&
            buf[i+3] == 'F' && buf[i+4] == 'r' && buf[i+5] == 'e' &&
            buf[i+6] == 'e' && buf[i+7] == ':')
            break;
    }
    if (i >= n - 8)
        return -1;

    /* Skip to digits */
    i += 8;
    while (i < n && (buf[i] == ' ' || buf[i] == '\t'))
        i++;

    /* Parse number using subtraction (no hardware divide on M0+) */
    int val = 0;
    while (i < n && buf[i] >= '0' && buf[i] <= '9') {
        int digit = buf[i] - '0';
        /* val = val * 10 + digit, using shifts: val*10 = val*8 + val*2 */
        val = (val << 3) + (val << 1) + digit;
        i++;
    }
    return val;
}

/* Create an orphan: vfork child, child vforks grandchild,
 * grandchild exits, child exits without reaping grandchild.
 * Grandchild becomes a zombie orphan → should be reparented to init. */
static void create_orphan(void)
{
    pid_t child = vfork();
    if (child == 0) {
        /* Child A: vfork grandchild B, then exit without reaping */
        pid_t grandchild = vfork();
        if (grandchild == 0) {
            /* Grandchild B: just exit */
            _exit(0);
        }
        /* Child A: do NOT waitpid(grandchild) — leave it as zombie */
        _exit(0);
    }
    /* Parent: reap child A */
    int status = 0;
    waitpid(child, &status, 0);
}

int main(void)
{
    int free_before = read_free_kb();
    UT_ASSERT(free_before > 0, "read /proc/meminfo baseline");

    /* Create orphans — if reparenting fails, the vfork/exit sequence
     * may hang or leak user_pages.  Stack pages of orphan zombies
     * are only freed when init reaps them (not tested here). */
    int i;
    for (i = 0; i < 3; i++)
        create_orphan();

    /* Completing without hang is the primary assertion.
     * Also check user_pages don't leak (freed in sys_exit).
     * Allow 3 * 4 kB = 12 kB tolerance for un-reaped zombie stack pages. */
    int free_after = read_free_kb();
    UT_ASSERT(free_after > 0, "read /proc/meminfo after orphans");

    int leaked = free_before - free_after;
    UT_ASSERT(leaked <= 12, "user_pages not leaked by orphan exits");

    UT_SUMMARY("test_orphan");
}
