#ifndef PPAP_COMMON_PTRACE_H
#define PPAP_COMMON_PTRACE_H

#include <stdint.h>

/* Linux-compatible core requests used by the initial PPAP tracer. */
#define PTRACE_TRACEME   0
#define PTRACE_GETREGS   12
#define PTRACE_CONT      7
#define PTRACE_DETACH    17
#define PTRACE_SYSCALL   24

/* PPAP-specific helper requests. */
#define PTRACE_GETEVENT  0x5000
#define PTRACE_SETMODE   0x5001

/* Runtime trace mode bits stored in the PCB. */
#define PPAP_TRACE_MODE_PPAP_SYSCALL  0x01
#define PPAP_TRACE_MODE_SUBSYS_CALL   0x02

/* Trace stop kinds returned by PTRACE_GETEVENT. */
#define PPAP_TRACE_EVENT_NONE           0
#define PPAP_TRACE_EVENT_EXEC           1
#define PPAP_TRACE_EVENT_SYSCALL_ENTER  2
#define PPAP_TRACE_EVENT_SYSCALL_EXIT   3
#define PPAP_TRACE_EVENT_SUBSYS_ENTER   4
#define PPAP_TRACE_EVENT_SUBSYS_EXIT    5

/* Event ABI tags. */
#define PPAP_TRACE_ABI_PPAP      0
#define PPAP_TRACE_ABI_H68K_DOS  1
#define PPAP_TRACE_ABI_H68K_IOCS 2
#define PPAP_TRACE_ABI_CPM_BDOS  3
#define PPAP_TRACE_ABI_CPM_BIOS  4

/* Register set kinds returned by PTRACE_GETREGS. */
#define PPAP_TRACE_REGSET_NONE  0
#define PPAP_TRACE_REGSET_ARM   1
#define PPAP_TRACE_REGSET_M68K  2
#define PPAP_TRACE_REGSET_Z80   3
#define PPAP_PTRACE_REGS_MAX    20

/* Event flags. */
#define PPAP_TRACE_FLAG_RESTART         0x01

struct ppap_ptrace_event {
    uint32_t event;
    uint32_t flags;
    uint32_t abi;
    uint32_t nr;
    uint32_t args[6];
    int32_t  ret;
};

struct ppap_ptrace_regs {
    uint32_t regset;
    uint32_t abi;
    uint32_t words;
    uint32_t regs[PPAP_PTRACE_REGS_MAX];
};

#endif /* PPAP_COMMON_PTRACE_H */
