/*
 * test_env.c — end-to-end env inheritance across execve
 *
 * Parent mode (no argv[1]) vforks a child that re-execs this same
 * program with a different envp for each sub-test.  Child mode
 * dispatches on argv[1] to a specific verification routine.
 *
 * Sub-tests:
 *   "ok"      — envp = {"PPAP_ENV_TEST=ok", NULL}.  Child uc_getenv
 *                must return "ok".
 *   "none"    — envp = NULL.  Child uc_getenv must return NULL and
 *                environ must be non-NULL but point at an empty list.
 *   "multi"   — envp = {"PPAP_A=a", "PPAP_B=bb", "PPAP_C=ccc", NULL}.
 *                Each lookup must return its specific value; an
 *                unrelated key returns NULL.
 *
 * Child exit 0 on pass, non-zero on each specific failure mode (see
 * constants below) — the parent records status via UT_ASSERT_EQ.
 */

#include "common/errno.h"
#include "lib/uclib.h"
#include "utest.h"

/* Child exit codes — distinct per failure mode so a regression points
 * straight at the broken check. */
#define CHILD_FAIL_MISSING 41      /* expected var not found */
#define CHILD_FAIL_WRONG_VALUE 42  /* var found but value mismatched */
#define CHILD_FAIL_UNEXPECTED 43   /* NULL-envp test saw any var */
#define CHILD_FAIL_ENVIRON_NULL 44 /* environ global never set */

static int check_ok(void) {
  const char *v = uc_getenv("PPAP_ENV_TEST");
  if (!v) return CHILD_FAIL_MISSING;
  if (v[0] != 'o' || v[1] != 'k' || v[2] != '\0') return CHILD_FAIL_WRONG_VALUE;
  return 0;
}

static int check_none(void) {
  if (environ == 0) return CHILD_FAIL_ENVIRON_NULL;
  if (environ[0] != 0) return CHILD_FAIL_UNEXPECTED;
  if (uc_getenv("PPAP_ENV_TEST") != 0) return CHILD_FAIL_UNEXPECTED;
  return 0;
}

static int str_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == 0 && *b == 0;
}

static int check_multi(void) {
  const char *a = uc_getenv("PPAP_A");
  const char *b = uc_getenv("PPAP_B");
  const char *c = uc_getenv("PPAP_C");
  if (!a || !b || !c) return CHILD_FAIL_MISSING;
  if (!str_eq(a, "a") || !str_eq(b, "bb") || !str_eq(c, "ccc"))
    return CHILD_FAIL_WRONG_VALUE;
  if (uc_getenv("PPAP_MISSING") != 0) return CHILD_FAIL_UNEXPECTED;
  return 0;
}

static int dispatch_child(const char *mode) {
  if (mode[0] == 'o') return check_ok();
  if (mode[0] == 'n') return check_none();
  if (mode[0] == 'm') return check_multi();
  return CHILD_FAIL_UNEXPECTED;
}

static int run_sub_test(char *self, const char *mode, char *const *envp) {
  char *child_argv[] = {self, (char *)mode, 0};
  pid_t cpid = vfork();
  if (cpid < 0) return -1;
  if (cpid == 0) {
    execve(self, child_argv, envp);
    _exit(127);
  }
  int status = 0;
  if (waitpid(cpid, &status, 0) != cpid) return -2;
  if (!WIFEXITED(status)) return -3;
  return WEXITSTATUS(status);
}

int main(int argc, char *argv[]) {
  if (argc >= 2) return dispatch_child(argv[1]);

  /* Parent: each sub-test runs a fresh child with a tailored envp. */
  char *envp_ok[] = {(char *)"PPAP_ENV_TEST=ok", 0};
  UT_ASSERT_EQ(run_sub_test(argv[0], "ok", envp_ok), 0);

  UT_ASSERT_EQ(run_sub_test(argv[0], "none", 0), 0);

  char *envp_multi[] = {(char *)"PPAP_A=a", (char *)"PPAP_B=bb",
                        (char *)"PPAP_C=ccc", 0};
  UT_ASSERT_EQ(run_sub_test(argv[0], "multi", envp_multi), 0);

  UT_SUMMARY("test_env");
}
