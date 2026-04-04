/*
 * test_h68k_path.c — Unit tests for Human68k path translation and error mapping
 *
 * Tests h68k_translate_path() and h68k_errno() from h68k_util.c.
 */

#include "test_framework.h"
#include "kernel/core/subsys/h68k_util.h"
#include "kernel/common/errno.h"
#include <string.h>

/* ── Path translation tests ──────────────────────────────────────────── */

static void test_drive_letter_absolute(void)
{
    char buf[128];
    int r = h68k_translate_path("A:\\DIR\\FILE.X", buf, sizeof(buf));
    ASSERT(r > 0, "should succeed");
    ASSERT(strcmp(buf, "/a/DIR/FILE.X") == 0, buf);
}

static void test_drive_letter_lowercase(void)
{
    char buf[128];
    h68k_translate_path("B:\\folder\\data", buf, sizeof(buf));
    ASSERT(strcmp(buf, "/b/folder/data") == 0, buf);
}

static void test_drive_letter_uppercase(void)
{
    char buf[128];
    h68k_translate_path("C:\\HELLO", buf, sizeof(buf));
    ASSERT(strcmp(buf, "/c/HELLO") == 0, buf);
}

static void test_drive_root(void)
{
    char buf[128];
    h68k_translate_path("A:\\", buf, sizeof(buf));
    ASSERT(strcmp(buf, "/a/") == 0, buf);
}

static void test_drive_no_backslash(void)
{
    char buf[128];
    h68k_translate_path("A:FILE.X", buf, sizeof(buf));
    ASSERT(strcmp(buf, "/a/FILE.X") == 0, buf);
}

static void test_absolute_no_drive(void)
{
    char buf[128];
    h68k_translate_path("\\DIR\\FILE", buf, sizeof(buf));
    ASSERT(strcmp(buf, "/DIR/FILE") == 0, buf);
}

static void test_relative_path(void)
{
    char buf[128];
    h68k_translate_path("DIR\\FILE.X", buf, sizeof(buf));
    ASSERT(strcmp(buf, "DIR/FILE.X") == 0, buf);
}

static void test_bare_filename(void)
{
    char buf[128];
    h68k_translate_path("FILE.X", buf, sizeof(buf));
    ASSERT(strcmp(buf, "FILE.X") == 0, buf);
}

static void test_empty_string(void)
{
    char buf[128];
    int r = h68k_translate_path("", buf, sizeof(buf));
    ASSERT_EQ(r, 0);
    ASSERT(strcmp(buf, "") == 0, buf);
}

static void test_forward_slashes_passthrough(void)
{
    char buf[128];
    h68k_translate_path("A:/dir/file", buf, sizeof(buf));
    ASSERT(strcmp(buf, "/a/dir/file") == 0, buf);
}

static void test_nested_dirs(void)
{
    char buf[128];
    h68k_translate_path("A:\\one\\two\\three\\file.txt", buf, sizeof(buf));
    ASSERT(strcmp(buf, "/a/one/two/three/file.txt") == 0, buf);
}

static void test_buffer_overflow_returns_neg1(void)
{
    char buf[4];  /* too small */
    int r = h68k_translate_path("A:\\DIR\\FILE", buf, sizeof(buf));
    ASSERT_EQ(r, -1);
}

static void test_tight_fit(void)
{
    char buf[7];  /* exactly fits "/a/FOO" + NUL */
    int r = h68k_translate_path("A:\\FOO", buf, sizeof(buf));
    ASSERT(r > 0, "should fit");
    ASSERT(strcmp(buf, "/a/FOO") == 0, buf);
}

/* ── Error code translation tests ────────────────────────────────────── */

static void test_errno_positive_passthrough(void)
{
    ASSERT_EQ(h68k_errno(0), 0);
    ASSERT_EQ(h68k_errno(5), 5);
    ASSERT_EQ(h68k_errno(42), 42);
}

static void test_errno_enoent(void)
{
    ASSERT_EQ(h68k_errno(-ENOENT), -2);
}

static void test_errno_ebadf(void)
{
    ASSERT_EQ(h68k_errno(-EBADF), -6);
}

static void test_errno_enomem(void)
{
    ASSERT_EQ(h68k_errno(-ENOMEM), -8);
}

static void test_errno_eacces(void)
{
    ASSERT_EQ(h68k_errno(-EACCES), -13);
}

static void test_errno_eexist(void)
{
    ASSERT_EQ(h68k_errno(-EEXIST), -80);
}

static void test_errno_enosys(void)
{
    ASSERT_EQ(h68k_errno(-ENOSYS), -1);
}

static void test_errno_unknown(void)
{
    ASSERT_EQ(h68k_errno(-999), -1);
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== test_h68k_path ===\n");

    TEST_GROUP("Path translation");
    RUN_TEST(test_drive_letter_absolute);
    RUN_TEST(test_drive_letter_lowercase);
    RUN_TEST(test_drive_letter_uppercase);
    RUN_TEST(test_drive_root);
    RUN_TEST(test_drive_no_backslash);
    RUN_TEST(test_absolute_no_drive);
    RUN_TEST(test_relative_path);
    RUN_TEST(test_bare_filename);
    RUN_TEST(test_empty_string);
    RUN_TEST(test_forward_slashes_passthrough);
    RUN_TEST(test_nested_dirs);
    RUN_TEST(test_buffer_overflow_returns_neg1);
    RUN_TEST(test_tight_fit);

    TEST_GROUP("Error code translation");
    RUN_TEST(test_errno_positive_passthrough);
    RUN_TEST(test_errno_enoent);
    RUN_TEST(test_errno_ebadf);
    RUN_TEST(test_errno_enomem);
    RUN_TEST(test_errno_eacces);
    RUN_TEST(test_errno_eexist);
    RUN_TEST(test_errno_enosys);
    RUN_TEST(test_errno_unknown);

    TEST_SUMMARY();
}
