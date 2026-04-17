# =============================================================================
# syscall.S — ecall syscall stubs for PPAP user-space programs (RISC-V)
# =============================================================================
#
# RISC-V syscall convention (matches Linux RISC-V ABI):
#   a7  = syscall number
#   a0–a5 = arguments
#   a0  = return value (negative errno on error)
#
# ecall triggers the environment call exception (mcause=11 from M-mode,
# mcause=8 from U-mode).
#
# PPAP syscall numbers: 16-bit, high byte = group, low byte = index.
# =============================================================================

# Macro for simple syscalls (up to 4 args in a0-a3, already in place)
.macro SYSCALL_STUB name, nr
.section .text.\name, "ax"
.global \name
\name:
    li   a7, \nr
    ecall
    ret
.endm

# Macro for _exit (noreturn)
.section .text._exit, "ax"
.global _exit
_exit:
    li   a7, 0              # SYS_EXIT = 0x0000
    ecall
    j    .                  # should never return

# --- Process management ---
SYSCALL_STUB getpid,    0x0006
SYSCALL_STUB getppid,   0x0008
SYSCALL_STUB vfork,     0x0002
SYSCALL_STUB execve,    0x0003
SYSCALL_STUB waitpid,   0x0004
SYSCALL_STUB setsid,    0x000B
SYSCALL_STUB setpgid,   0x0009
SYSCALL_STUB getpgid,   0x000A
SYSCALL_STUB kill,       0x0600
SYSCALL_STUB sigaction,  0x0601
SYSCALL_STUB ptrace,    0x000F

# --- File I/O ---
SYSCALL_STUB read,      0x0100
SYSCALL_STUB write,     0x0101
SYSCALL_STUB open,      0x0102
SYSCALL_STUB close,     0x0103
SYSCALL_STUB dup,       0x0104
SYSCALL_STUB dup2,      0x0105
SYSCALL_STUB pipe,      0x0106
SYSCALL_STUB ioctl,     0x0107
SYSCALL_STUB readv,     0x0109
SYSCALL_STUB writev,    0x010A
SYSCALL_STUB lseek,     0x010B

# --- File system ---
SYSCALL_STUB stat,      0x0200
SYSCALL_STUB access,    0x0202
SYSCALL_STUB getcwd,    0x0203
SYSCALL_STUB mkdir,     0x0204
SYSCALL_STUB rmdir,     0x0205
SYSCALL_STUB unlink,    0x0206
SYSCALL_STUB chdir,     0x0207
SYSCALL_STUB readlink,  0x0208
SYSCALL_STUB getdents,  0x0300
SYSCALL_STUB statfs64,  0x0305

# --- Memory ---
SYSCALL_STUB brk,       0x0400

# --- Time ---
SYSCALL_STUB nanosleep,     0x0500
SYSCALL_STUB clock_gettime, 0x0501

# --- Poll ---
# ppoll is a 5-arg syscall; a4 is already in place for RISC-V (no stack arg)
SYSCALL_STUB ppoll,     0x0701

# --- System control ---
SYSCALL_STUB poweroff,  0x0B00
