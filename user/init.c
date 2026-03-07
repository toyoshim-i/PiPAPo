/*
 * init.c — Minimal init for PPAP (no libc, PIC, 2 SRAM pages)
 *
 * Parses /etc/inittab for "respawn" entries, forks+execs each,
 * and respawns them when they exit.
 *
 * Compiled with PIC (like other user programs) so string literals
 * are accessed via GOT.  Uses 2 SRAM pages (GOT + stack) vs ~24 KB
 * for the busybox init (musl libc GOT + heap + stack).
 *
 * See busybox.init for the full-featured BusyBox-based init.
 */

#include "syscall.h"

/* ── String helpers ────────────────────────────────────────────────────────── */

static int my_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void puts_fd(int fd, const char *s)
{
    write(fd, s, my_strlen(s));
}

static int streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ── Inittab structures ───────────────────────────────────────────────────── */

#define MAX_ENTRIES   4
#define MAX_ARGV      4
#define INITTAB_SIZE  256

struct entry {
    int   pid;                 /* running child PID (0 = not running) */
    char *argv[MAX_ARGV + 1];  /* parsed argv for execve (NULL-terminated) */
};

/* Parse a command string into argv[] (modifies cmd by inserting NULs) */
static int parse_argv(char *cmd, char *argv[], int max)
{
    int argc = 0;
    while (*cmd && argc < max) {
        while (*cmd == ' ') cmd++;
        if (!*cmd) break;
        argv[argc++] = cmd;
        while (*cmd && *cmd != ' ') cmd++;
        if (*cmd) *cmd++ = '\0';
    }
    argv[argc] = (char *)0;
    return argc;
}

/* Spawn a child process for an entry */
static void spawn(struct entry *e)
{
    pid_t pid = vfork();
    if (pid == 0) {
        /* child — exec the command */
        char *envp[] = { (char *)0 };
        execve(e->argv[0], e->argv, envp);
        _exit(127);
    }
    if (pid > 0)
        e->pid = pid;
}

/* ── main ──────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    char buf[INITTAB_SIZE];
    struct entry entries[MAX_ENTRIES];
    int n_entries = 0;

    /* 1. Read /etc/inittab */
    int fd = open("/etc/inittab", 0, 0);  /* O_RDONLY */
    if (fd < 0) {
        puts_fd(2, "init: no inittab, exec /bin/sh\n");
        char *sh_argv[] = { "/bin/sh", (char *)0 };
        char *sh_envp[] = { (char *)0 };
        execve("/bin/sh", sh_argv, sh_envp);
        _exit(1);
    }

    int n = read(fd, buf, INITTAB_SIZE - 1);
    close(fd);
    if (n <= 0) {
        puts_fd(2, "init: empty inittab\n");
        _exit(1);
    }
    buf[n] = '\0';

    /* 2. Parse inittab — format: id::action:command [args] */
    char *p = buf;
    while (*p && n_entries < MAX_ENTRIES) {
        /* Skip blank lines and comments */
        if (*p == '\n') { p++; continue; }
        if (*p == '#')  { while (*p && *p != '\n') p++; continue; }

        /* Field 1: id (skip) */
        while (*p && *p != ':' && *p != '\n') p++;
        if (*p != ':') { while (*p && *p != '\n') p++; continue; }
        p++;

        /* Field 2: runlevels (skip, usually empty) */
        while (*p && *p != ':' && *p != '\n') p++;
        if (*p != ':') { while (*p && *p != '\n') p++; continue; }
        p++;

        /* Field 3: action */
        char *action = p;
        while (*p && *p != ':' && *p != '\n') p++;
        if (*p != ':') { while (*p && *p != '\n') p++; continue; }
        *p++ = '\0';

        /* Field 4: command */
        char *cmd = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = '\0';

        if (streq(action, "respawn") && *cmd) {
            parse_argv(cmd, entries[n_entries].argv, MAX_ARGV);
            entries[n_entries].pid = 0;
            n_entries++;
        }
    }

    if (n_entries == 0) {
        puts_fd(2, "init: no respawn entries\n");
        _exit(1);
    }

    /* 3. Startup message */
    puts_fd(1, "init started\n");

    /* 4. Spawn initial processes */
    for (int i = 0; i < n_entries; i++)
        spawn(&entries[i]);

    /* 5. Wait/respawn loop */
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid > 0) {
            for (int i = 0; i < n_entries; i++) {
                if (entries[i].pid == pid) {
                    entries[i].pid = 0;
                    spawn(&entries[i]);
                    break;
                }
            }
        }
    }
}
