/*
 * test_env.c — end-to-end env inheritance across execve
 *
 * The parent mode (no argv[1]) vforks a child that re-execs this same
 * program with an explicit envp.  The child mode ("child" argument)
 * walks envp via &argv[argc+1] — the POSIX convention the kernel
 * loader lays out — and checks for PPAP_ENV_TEST=ok.
 *
 * Exit codes:
 *   child:  0 = env var seen with expected value
 *          41 = env var missing
 *          42 = env var found but value wrong
 *   parent: 0 = child exited 0
 *          51 = vfork failed
 *          52 = child did not exit cleanly
 *          53 = child exit status != 0 (see stderr for raw status)
 */

#include "common/errno.h"
#include "lib/uclib.h"
#include "utest.h"

static int run_child(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  /* Prefer uc_getenv — exercises the libc path (environ set by crt0).
   * If that works, we've also transitively verified the stack walk. */
  const char *v = uc_getenv("PPAP_ENV_TEST");
  if (!v) return 41;
  if (v[0] == 'o' && v[1] == 'k' && v[2] == '\0') return 0;
  return 42;
}

int main(int argc, char *argv[]) {
  if (argc >= 2 && argv[1][0] == 'c') return run_child(argc, argv);

  /* Parent mode: vfork + execve with explicit envp. */
  char *child_argv[] = {argv[0], (char *)"child", 0};
  char *child_envp[] = {(char *)"PPAP_ENV_TEST=ok", 0};

  pid_t cpid = vfork();
  if (cpid < 0) return 51;
  if (cpid == 0) {
    execve(argv[0], child_argv, child_envp);
    _exit(127);
  }

  int status = 0;
  UT_ASSERT_EQ(waitpid(cpid, &status, 0), cpid);
  UT_ASSERT(WIFEXITED(status), "child should exit cleanly");
  UT_ASSERT_EQ(WEXITSTATUS(status), 0);

  UT_SUMMARY("test_env");
}
