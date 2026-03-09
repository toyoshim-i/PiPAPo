/*
 * test_stat.c — stat, getdents on romfs
 */

#include "utest.h"

int main(void)
{
    /* 1. stat a known file */
    struct stat st;
    int ret = stat("/bin/test_stat", &st);
    UT_ASSERT_EQ(ret, 0);
    UT_ASSERT(S_ISREG(st.st_mode), "test_stat is regular file");
    UT_ASSERT(st.st_size > 0, "test_stat has nonzero size");

    /* 2. stat a directory */
    ret = stat("/bin", &st);
    UT_ASSERT_EQ(ret, 0);
    UT_ASSERT(S_ISDIR(st.st_mode), "/bin is directory");

    /* 3. stat root */
    ret = stat("/", &st);
    UT_ASSERT_EQ(ret, 0);
    UT_ASSERT(S_ISDIR(st.st_mode), "/ is directory");

    /* 4. stat nonexistent */
    ret = stat("/no/such/file", &st);
    UT_ASSERT(ret < 0, "stat nonexistent fails");

    /* 5. getdents on / — should find "bin" */
    int fd = open("/", O_RDONLY, 0);
    UT_ASSERT(fd >= 0, "open / for getdents");
    if (fd >= 0) {
        struct dirent entries[8];
        int n = getdents(fd, entries, 8);
        UT_ASSERT(n > 0, "getdents / returns entries");

        /* Look for "bin" in entries */
        int found_bin = 0;
        int i;
        for (i = 0; i < n; i++) {
            /* strcmp inline */
            const char *a = entries[i].d_name;
            const char *b = "bin";
            int eq = 1;
            while (*a && *b) { if (*a != *b) { eq = 0; break; } a++; b++; }
            if (*a != *b) eq = 0;
            if (eq) { found_bin = 1; break; }
        }
        UT_ASSERT(found_bin, "getdents / contains bin");
        close(fd);
    }

    /* 6. getdents on /bin — should find test_stat */
    fd = open("/bin", O_RDONLY, 0);
    UT_ASSERT(fd >= 0, "open /bin for getdents");
    if (fd >= 0) {
        struct dirent entries[32];
        int n = getdents(fd, entries, 32);
        UT_ASSERT(n > 0, "getdents /bin returns entries");

        int found_self = 0;
        int i;
        for (i = 0; i < n; i++) {
            const char *a = entries[i].d_name;
            const char *b = "test_stat";
            int eq = 1;
            while (*a && *b) { if (*a != *b) { eq = 0; break; } a++; b++; }
            if (*a != *b) eq = 0;
            if (eq) { found_self = 1; break; }
        }
        UT_ASSERT(found_self, "getdents /bin contains test_stat");
        close(fd);
    }

    UT_SUMMARY("test_stat");
}
