#ifndef PPAP_COMMON_PTRACE_H
#define PPAP_COMMON_PTRACE_H

#include <stdint.h>

/* Linux-compatible core requests used by the initial PPAP tracer. */
#define PTRACE_TRACEME   0
#define PTRACE_CONT      7
#define PTRACE_DETACH    17
#define PTRACE_SYSCALL   24

/* PPAP-specific helper request: copy the last stop event to user memory. */
#define PTRACE_GETEVENT  0x5000

/* Runtime trace mode bits stored in the PCB. */
#define PPAP_TRACE_MODE_PPAP_SYSCALL  0x01

/* Trace stop kinds returned by PTRACE_GETEVENT. */
#define PPAP_TRACE_EVENT_NONE           0
#define PPAP_TRACE_EVENT_EXEC           1
#define PPAP_TRACE_EVENT_SYSCALL_ENTER  2
#define PPAP_TRACE_EVENT_SYSCALL_EXIT   3

/* Event flags. */
#define PPAP_TRACE_FLAG_RESTART         0x01

struct ppap_ptrace_event {
    uint32_t event;
    uint32_t flags;
    uint32_t nr;
    uint32_t args[6];
    int32_t  ret;
};

#endif /* PPAP_COMMON_PTRACE_H */
