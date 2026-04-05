/*
 * common/core/signal_defs.h — Signal number constants
 *
 * Extracted from core/signal/signal.h for cross-module use.
 * Constants only — no function declarations.
 */

#ifndef PPAP_COMMON_SIGNAL_H
#define PPAP_COMMON_SIGNAL_H

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGTRAP 5
#define SIGKILL 9
#define SIGUSR1 10
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19

#define NSIG 32

#endif /* PPAP_COMMON_SIGNAL_H */
