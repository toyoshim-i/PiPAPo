#ifndef PPAP_COMMON_PTRACE_H
#define PPAP_COMMON_PTRACE_H

#include <stdint.h>

/* Linux-compatible core requests used by the initial PPAP tracer. */
#define PTRACE_TRACEME   0
#define PTRACE_PEEKDATA  2
#define PTRACE_POKEDATA  5
#define PTRACE_GETREGS   12
#define PTRACE_SETREGS   13
#define PTRACE_ATTACH    16
#define PTRACE_SINGLESTEP 9
#define PTRACE_CONT      7
#define PTRACE_DETACH    17
#define PTRACE_SYSCALL   24

/*
 * PPAP PEEKDATA / POKEDATA ABI:
 *   - addr points into the stopped tracee
 *   - data points to a tracer-owned 32-bit word
 *   - PEEKDATA copies a target word into *data
 *   - POKEDATA copies *data into the target word
 *
 * This intentionally differs from Linux's return-value-based PEEKDATA
 * because PPAP syscalls report errors as negative return values.
 */

/* PPAP-specific helper requests. */
#define PTRACE_GETEVENT  0x5000
#define PTRACE_SETMODE   0x5001
#define PTRACE_GETCAPS   0x5002
#define PTRACE_SETBP     0x5003
#define PTRACE_CLRBP     0x5004
#define PTRACE_GETSURFACE 0x5005
#define PTRACE_SETSURFACE 0x5006

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
#define PPAP_TRACE_EVENT_DEBUG_STOP     6

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

/* Active debug surface kind returned by PTRACE_GETSURFACE. */
#define PPAP_TRACE_SURFACE_REAL  0
#define PPAP_TRACE_SURFACE_ECPU  1
#define PPAP_PTRACE_SURFACE_MASK_REAL  (1u << PPAP_TRACE_SURFACE_REAL)
#define PPAP_PTRACE_SURFACE_MASK_ECPU  (1u << PPAP_TRACE_SURFACE_ECPU)

/* Capability bits returned by PTRACE_GETCAPS. */
#define PPAP_PTRACE_CAP_GETREGS   (1u << 0)
#define PPAP_PTRACE_CAP_SETREGS   (1u << 1)
#define PPAP_PTRACE_CAP_PEEKPOKE  (1u << 2)
#define PPAP_PTRACE_CAP_SINGLESTEP (1u << 3)
#define PPAP_PTRACE_CAP_SW_BP     (1u << 4)
#define PPAP_PTRACE_CAP_HW_BP     (1u << 5)

/* Event flags. */
#define PPAP_TRACE_FLAG_RESTART         0x01
#define PPAP_DEBUG_STOP_STEP            0x0001
#define PPAP_DEBUG_STOP_SW_BP           0x0002
#define PPAP_DEBUG_STOP_HW_BP           0x0004

/* Breakpoint flags for ppap_ptrace_bp.flags. */
#define PPAP_PTRACE_BP_SW               0x0001
#define PPAP_PTRACE_BP_HW               0x0002

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

struct ppap_ptrace_caps {
    uint32_t regset;
    uint32_t abi;
    uint32_t surface;
    uint32_t surfaces;
    uint32_t caps;
    uint32_t max_bps;
};

struct ppap_ptrace_bp {
    int32_t  id;
    uint32_t addr;
    uint32_t flags;
};

#endif /* PPAP_COMMON_PTRACE_H */
