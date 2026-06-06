/*
 * test_vfork.c — vfork + execve + waitpid integration test
 *
 * Runs one basic vfork/execve/waitpid handoff, then repeats the handoff a
 * few times.  The split keeps simple vfork failures distinct from repeated
 * parent-block/child-runnable publication issues that real hardware can
 * preempt.
 */

#include "utest.h"

#define VFORK_EXEC_ITERATIONS 8

static pid_t spawn_hello_with_quiet_stdout(void) {
  int saved_stdout = dup(1);
  int nullfd = open("/dev/null", O_WRONLY, 0);
  if (saved_stdout >= 0 && nullfd >= 0) dup2(nullfd, 1);
  if (nullfd >= 0) close(nullfd);

  pid_t pid = vfork();

  if (pid == 0) {
    execve("/bin/hello", (void *)0, (void *)0);
    _exit(1);
  }

  if (saved_stdout >= 0) {
    dup2(saved_stdout, 1);
    close(saved_stdout);
  }

  return pid;
}

static void check_basic_vfork_exec(void) {
  pid_t pid = spawn_hello_with_quiet_stdout();
  int status = 0;
  UT_ASSERT(pid > 0, "basic vfork should return child pid");
  UT_ASSERT_EQ(waitpid(pid, &status, 0), pid);
  UT_ASSERT(WIFEXITED(status), "basic vfork exec child should exit");
  UT_ASSERT_EQ(WEXITSTATUS(status), 0);
}

static void check_stress_vfork_exec_once(void) {
  pid_t pid = spawn_hello_with_quiet_stdout();
  int status = 0;
  UT_ASSERT(pid > 0, "stress vfork should return child pid");
  UT_ASSERT_EQ(waitpid(pid, &status, 0), pid);
  UT_ASSERT(WIFEXITED(status), "stress vfork exec child should exit");
  UT_ASSERT_EQ(WEXITSTATUS(status), 0);
}

int main(void) {
  check_basic_vfork_exec();

  for (int i = 0; i < VFORK_EXEC_ITERATIONS; i++)
    check_stress_vfork_exec_once();

  UT_SUMMARY("test_vfork");
}
