/*
 * kill.c — send a signal to a process
 *
 * Usage:
 *   kill [-SIG] PID...        send SIG (default TERM) to each PID
 *   kill -l                   list known signals by name/number
 *
 * SIG may be a number (9) or a name with or without SIG prefix
 * (KILL, SIGKILL).  Signal set mirrors src/common/signal.h.
 */

#include "common/signal.h"
#include "lib/uclib.h"

struct sig_ent {
  const char *name; /* without SIG prefix */
  int num;
};

static const struct sig_ent sigs[] = {
    {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT},
    {"TRAP", SIGTRAP}, {"KILL", SIGKILL}, {"USR1", SIGUSR1},
    {"USR2", SIGUSR2}, {"PIPE", SIGPIPE}, {"TERM", SIGTERM},
    {"CHLD", SIGCHLD}, {"CONT", SIGCONT}, {"STOP", SIGSTOP},
};
static const int nsigs = sizeof(sigs) / sizeof(sigs[0]);

static int parse_unsigned(const char *s, int *out) {
  if (!*s) return -1;
  int v = 0;
  while (*s) {
    if (*s < '0' || *s > '9') return -1;
    v = v * 10 + (*s - '0');
    s++;
  }
  *out = v;
  return 0;
}

static int parse_signed(const char *s, int *out) {
  int neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  }
  int v;
  if (parse_unsigned(s, &v) < 0) return -1;
  *out = neg ? -v : v;
  return 0;
}

/* Match a signal spec: bare number, or name with/without SIG prefix. */
static int parse_sig(const char *s, int *out) {
  int n;
  if (parse_unsigned(s, &n) == 0) {
    if (n < 0 || n > 31) return -1;
    *out = n;
    return 0;
  }
  const char *name = s;
  if (name[0] == 'S' && name[1] == 'I' && name[2] == 'G') name += 3;
  for (int i = 0; i < nsigs; i++) {
    if (uc_strcmp(sigs[i].name, name) == 0) {
      *out = sigs[i].num;
      return 0;
    }
  }
  return -1;
}

static void list_signals(void) {
  for (int i = 0; i < nsigs; i++) {
    uc_printf("%2d) SIG%s\n", sigs[i].num, sigs[i].name);
  }
}

int main(int argc, char *argv[]) {
  int sig = SIGTERM;
  int argi = 1;

  if (argi < argc && uc_strcmp(argv[argi], "--help") == 0) {
    uc_puts(
        "Usage: kill [-SIG] PID...\n"
        "       kill -l\n");
    return 0;
  }

  if (argi < argc && uc_strcmp(argv[argi], "-l") == 0) {
    list_signals();
    return 0;
  }

  /* -s SIG form */
  if (argi < argc && uc_strcmp(argv[argi], "-s") == 0) {
    if (argi + 1 >= argc) {
      uc_eputs("kill: option -s requires an argument\n");
      return 1;
    }
    if (parse_sig(argv[argi + 1], &sig) < 0) {
      uc_eputs("kill: invalid signal: ");
      uc_eputs(argv[argi + 1]);
      uc_eputs("\n");
      return 1;
    }
    argi += 2;
  } else if (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
    /* -SIG form (e.g. -9, -KILL, -SIGKILL) */
    if (parse_sig(argv[argi] + 1, &sig) < 0) {
      uc_eputs("kill: invalid signal: ");
      uc_eputs(argv[argi]);
      uc_eputs("\n");
      return 1;
    }
    argi++;
  }

  if (argi >= argc) {
    uc_eputs("kill: missing operand\n");
    return 1;
  }

  int rc = 0;
  for (int i = argi; i < argc; i++) {
    int pid;
    if (parse_signed(argv[i], &pid) < 0) {
      uc_eputs("kill: invalid pid: ");
      uc_eputs(argv[i]);
      uc_eputs("\n");
      rc = 1;
      continue;
    }
    if (kill((pid_t)pid, sig) < 0) {
      uc_eputs("kill: (");
      uc_eputs(argv[i]);
      uc_eputs(") - no such process\n");
      rc = 1;
    }
  }
  return rc;
}
