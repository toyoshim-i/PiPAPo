/*
 * test_fs.c — Filesystem syscalls: open, close, read, lseek, getcwd,
 *             chdir, access, and dynamic mount lifetime
 */

#include "utest.h"

/* Helper: compare two strings, return 1 if equal */
static int streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

int main(void)
{
    /* 1. getcwd returns "/" initially */
    char cwd[64];
    int i;
    for (i = 0; i < 64; i++) cwd[i] = 0;
    int ret = getcwd(cwd, 64);
    UT_ASSERT(ret >= 0, "getcwd succeeds");
    UT_ASSERT(streq(cwd, "/"), "initial cwd is /");

    /* 2. open a known romfs file (self binary is in /bin/) */
    int fd = open("/bin/test_fs", O_RDONLY, 0);
    UT_ASSERT(fd >= 0, "open /bin/test_fs");

    /* 3. read from the file should return data */
    char buf[16];
    for (i = 0; i < 16; i++) buf[i] = 0;
    ssize_t n = read(fd, buf, 16);
    UT_ASSERT(n > 0, "read returns data");

    /* 4. lseek back to start */
    int pos = lseek(fd, 0, SEEK_SET);
    UT_ASSERT_EQ(pos, 0);

    /* 5. read again should return same data */
    char buf2[16];
    for (i = 0; i < 16; i++) buf2[i] = 0;
    ssize_t n2 = read(fd, buf2, 16);
    UT_ASSERT_EQ(n2, n);
    int match = 1;
    for (i = 0; i < n && i < 16; i++)
        if (buf[i] != buf2[i]) match = 0;
    UT_ASSERT(match, "re-read matches first read");

    /* 6. lseek to end, read returns 0 (EOF) */
    int end = lseek(fd, 0, SEEK_END);
    UT_ASSERT(end > 0, "lseek SEEK_END returns positive offset");
    ssize_t n3 = read(fd, buf, 16);
    UT_ASSERT_EQ(n3, 0);

    close(fd);

    /* 7. open non-existent file should fail */
    int bad = open("/no/such/file", O_RDONLY, 0);
    UT_ASSERT(bad < 0, "open nonexistent fails");

    /* 8. access on existing file */
    ret = access("/bin/test_fs", F_OK);
    UT_ASSERT_EQ(ret, 0);

    /* 9. access on non-existent file should fail */
    ret = access("/no/such/file", F_OK);
    UT_ASSERT(ret < 0, "access nonexistent fails");

    /* 10. chdir to /bin */
    ret = chdir("/bin");
    UT_ASSERT_EQ(ret, 0);

    /* 11. getcwd should now return /bin */
    for (i = 0; i < 64; i++) cwd[i] = 0;
    ret = getcwd(cwd, 64);
    UT_ASSERT(ret >= 0, "getcwd after chdir");
    UT_ASSERT(streq(cwd, "/bin"), "cwd is /bin after chdir");

    /* 12. chdir to non-existent dir should fail */
    ret = chdir("/no/such/dir");
    UT_ASSERT(ret < 0, "chdir nonexistent fails");

    /* 13. chdir back to / */
    ret = chdir("/");
    UT_ASSERT_EQ(ret, 0);

    /* 14. close invalid fd */
    ret = close(999);
    UT_ASSERT(ret < 0, "close bad fd fails");

    /* 15. a dynamic mount reserves its target until it is unmounted */
    ret = mount(0, "/vfs_test", "procfs", 0, 0);
    UT_ASSERT_EQ(ret, 0);
    ret = mount(0, "/vfs_test", "procfs", 0, 0);
    UT_ASSERT(ret < 0, "duplicate mount fails");

    /* 16. an open mount root keeps the mount busy */
    fd = open("/vfs_test", O_RDONLY, 0);
    UT_ASSERT(fd >= 0, "open mounted root");
    ret = umount2("/vfs_test", 0);
    UT_ASSERT(ret < 0, "umount open root fails");
    if (fd >= 0) close(fd);
    ret = umount2("/vfs_test", 0);
    UT_ASSERT_EQ(ret, 0);

    /* 17. statfs releases its transient mount reference */
    struct statfs sf;
    ret = mount(0, "/vfs_test", "procfs", 0, 0);
    UT_ASSERT_EQ(ret, 0);
    ret = statfs64("/vfs_test", sizeof(sf), &sf);
    UT_ASSERT_EQ(ret, 0);
    ret = umount2("/vfs_test", 0);
    UT_ASSERT_EQ(ret, 0);

    UT_SUMMARY("test_fs");
}
