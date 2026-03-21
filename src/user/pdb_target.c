#include "pdb_internal.h"
#include "pdb_trace_util.h"
#include "pdb_util.h"

int wait_child(pid_t pid, int *stopped, int *exit_code,
               struct ppap_ptrace_event *ev, int report_event)
{
    int status = 0;
    pid_t rc = waitpid(pid, &status, WSTOPPED);

    if (rc != pid) {
        put_err("pdb: waitpid failed\n");
        return -1;
    }

    if (WIFEXITED(status)) {
        *stopped = 0;
        *exit_code = WEXITSTATUS(status);
        if (report_event) {
            put_str("child exited ");
            put_u32((uint32_t)*exit_code);
            put_chr('\n');
        }
        return 1;
    }

    if (!WIFSTOPPED(status)) {
        *stopped = 0;
        put_err("pdb: unexpected wait status\n");
        return -1;
    }

    *stopped = 1;
    if (ptrace(PTRACE_GETEVENT, pid, (void *)0, ev) < 0) {
        put_err("pdb: GETEVENT failed\n");
        return -1;
    }
    if (report_event)
        print_event(ev);
    return 0;
}

int ensure_child_stopped(int child_stopped)
{
    if (child_stopped)
        return 1;
    put_err("pdb: child is not stopped\n");
    return 0;
}

int get_regs_if_stopped(pid_t pid, int child_stopped,
                        struct ppap_ptrace_regs *regs)
{
    if (!ensure_child_stopped(child_stopped))
        return 0;
    if (ptrace(PTRACE_GETREGS, pid, (void *)0, regs) < 0) {
        put_err("pdb: GETREGS failed\n");
        return 0;
    }
    return 1;
}

int get_caps_if_stopped(pid_t pid, int child_stopped,
                        struct ppap_ptrace_caps *caps)
{
    if (!ensure_child_stopped(child_stopped))
        return 0;
    if (ptrace(PTRACE_GETCAPS, pid, (void *)0, caps) < 0) {
        put_err("pdb: GETCAPS failed\n");
        return 0;
    }
    return 1;
}

void init_local_bp_table(pdb_local_bp_t *local_bp)
{
    for (int i = 0; i < PDB_LOCAL_BP_MAX; i++) {
        local_bp[i].used = 0;
        local_bp[i].enabled = 0;
        local_bp[i].addr = 0;
        local_bp[i].flags = 0;
    }
}

int start_tracee(char *argv[], int argi, int attach_mode, pid_t attach_pid,
                 pid_t *pid_out)
{
    pid_t pid = 0;

    if (attach_mode) {
        long rc;
        pid = attach_pid;
        rc = ptrace(PTRACE_ATTACH, pid, (void *)0, (void *)0);
        if (rc < 0) {
            put_err("pdb: ATTACH failed rc=");
            put_i32((int32_t)rc);
            put_chr('\n');
            return 0;
        }
    } else {
        pid = vfork();
        if (pid == 0) {
            ptrace(PTRACE_TRACEME, 0, (void *)0, (void *)0);
            execve(argv[argi], &argv[argi], (void *)0);
            _exit(127);
        }
        if (pid < 0) {
            put_err("pdb: vfork failed\n");
            return 0;
        }
    }

    *pid_out = pid;
    return 1;
}

/* Returns 1 when command completed, 2 when caller should return main_exit_code. */
static int resume_and_wait(pdb_dispatch_ctx_t *ctx, int request,
                           const char *request_name)
{
    pid_t pid = ctx->pid;
    int *child_stopped = ctx->child_stopped;
    int *child_exit_code = ctx->child_exit_code;
    struct ppap_ptrace_event *last_ev = ctx->last_ev;
    int batch_mode = ctx->batch_mode;
    int *main_exit_code = ctx->main_exit_code;
    long rc = ptrace(request, pid, (void *)0, (void *)0);
    int wr;
    if (rc < 0) {
        put_err("pdb: ");
        put_err(request_name);
        put_err(" failed rc=");
        put_i32((int32_t)rc);
        put_chr('\n');
        return 1;
    }
    *child_stopped = 0;
    wr = wait_child(pid, child_stopped, child_exit_code, last_ev, !batch_mode);
    if (wr < 0) {
        *main_exit_code = 1;
        return 2;
    }
    if (wr > 0) {
        *main_exit_code = *child_exit_code;
        return 2;
    }
    return 1;
}

/* Returns: 0=not handled, 1=handled, 2=caller should return main_exit_code. */
int handle_run_control_commands(pdb_dispatch_ctx_t *ctx, char **tok, int ntok)
{
    pid_t pid = ctx->pid;
    int *child_stopped = ctx->child_stopped;
    struct ppap_ptrace_caps *caps = ctx->caps;
    pdb_local_bp_t *local_bp = ctx->local_bp;

    if (streq(tok[0], "next") || streq(tok[0], "n")) {
        struct ppap_ptrace_regs regs;
        uint8_t op = 0;
        uint32_t pc = 0;
        uint32_t next_pc = 0;
        uint32_t next_bp_flag = PPAP_PTRACE_BP_SW;
        int use_temp_bp = 0;
        int has_enabled_bp = 0;
        int temp_bp_id = -1;
        long rc;
        if (ntok != 1) {
            put_err("pdb: usage: next\n");
            return 1;
        }
        if (!get_regs_if_stopped(pid, *child_stopped, &regs))
            return 1;
        if (ptrace(PTRACE_GETCAPS, pid, (void *)0, caps) == 0) {
            uint32_t cap_flag = select_bp_flag_from_caps(caps->caps);
            if (cap_flag == 0) {
                put_err("pdb: break not supported on this target/mapping\n");
                return 1;
            }
            next_bp_flag = cap_flag;
        }
        if (regs.regset == PPAP_TRACE_REGSET_Z80) {
            pc = regs.regs[7];  /* Z80 PC */
            rc = (long)peek_u8(pid, pc, &op);
            if (rc < 0) {
                put_err("pdb: next opcode read failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                return 1;
            }

            if (z80_is_call_opcode(op)) {
                next_pc = pc + 3u;
                use_temp_bp = 1;
                for (int i = 0; i < PDB_LOCAL_BP_MAX; i++) {
                    if (!local_bp[i].used || !local_bp[i].enabled)
                        continue;
                    if (local_bp[i].addr == next_pc) {
                        has_enabled_bp = 1;
                        break;
                    }
                }

                if (!has_enabled_bp) {
                    struct ppap_ptrace_bp bp;
                    bp.id = -1;
                    bp.addr = next_pc;
                    bp.flags = next_bp_flag;
                    rc = ptrace(PTRACE_SETBP, pid, (void *)0, &bp);
                    if (rc < 0) {
                        put_err("pdb: NEXT SETBP failed rc=");
                        put_i32((int32_t)rc);
                        put_chr('\n');
                        return 1;
                    }
                    temp_bp_id = bp.id;
                }
            }
        }

        if (use_temp_bp) {
            int rr = resume_and_wait(ctx, PTRACE_CONT, "CONT");
            if (*child_stopped && temp_bp_id >= 0) {
                struct ppap_ptrace_bp bp;
                bp.id = temp_bp_id;
                bp.addr = 0;
                bp.flags = 0;
                (void)ptrace(PTRACE_CLRBP, pid, (void *)0, &bp);
            }
            return rr;
        }
        return resume_and_wait(ctx, PTRACE_SINGLESTEP, "SINGLESTEP");
    }

    if (streq(tok[0], "step") || streq(tok[0], "s")) {
        if (ntok != 1) {
            put_err("pdb: usage: step\n");
            return 1;
        }
        if (!*child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        return resume_and_wait(ctx, PTRACE_SINGLESTEP, "SINGLESTEP");
    }

    if (streq(tok[0], "run") || streq(tok[0], "cont") ||
        streq(tok[0], "continue") || streq(tok[0], "c")) {
        if (ntok != 1) {
            put_err("pdb: usage: cont\n");
            return 1;
        }
        if (!*child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        return resume_and_wait(ctx, PTRACE_CONT, "CONT");
    }

    return 0;
}

int handle_session_commands(pdb_dispatch_ctx_t *ctx, char **tok, int ntok)
{
    pid_t pid = ctx->pid;
    int attach_mode = ctx->attach_mode;
    int *child_stopped = ctx->child_stopped;
    int *done = ctx->done;
    pdb_local_bp_t *local_bp = ctx->local_bp;

    if (streq(tok[0], "detach")) {
        long rc;
        if (ntok != 1) {
            put_err("pdb: usage: detach\n");
            return 1;
        }
        if (!*child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        if (local_bp) {
            for (int i = 0; i < PDB_LOCAL_BP_MAX; i++) {
                struct ppap_ptrace_bp bp;
                if (!local_bp[i].used || !local_bp[i].enabled)
                    continue;
                bp.id = i;
                bp.addr = 0;
                bp.flags = 0;
                if (ptrace(PTRACE_CLRBP, pid, (void *)0, &bp) == 0)
                    local_bp[i].enabled = 0;
            }
        }
        rc = ptrace(PTRACE_DETACH, pid, (void *)0, (void *)0);
        if (rc < 0) {
            put_err("pdb: DETACH failed rc=");
            put_i32((int32_t)rc);
            put_chr('\n');
            return 1;
        }
        *child_stopped = 0;
        put_str("detached\n");
        if (!attach_mode) {
            (void)kill(pid, 9);
            (void)waitpid(pid, (void *)0, 0);
        }
        *done = 1;
        return 1;
    }

    if (streq(tok[0], "quit") || streq(tok[0], "q")) {
        if (ntok != 1) {
            put_err("pdb: usage: quit\n");
            return 1;
        }
        if (*child_stopped) {
            if (local_bp) {
                for (int i = 0; i < PDB_LOCAL_BP_MAX; i++) {
                    struct ppap_ptrace_bp bp;
                    if (!local_bp[i].used || !local_bp[i].enabled)
                        continue;
                    bp.id = i;
                    bp.addr = 0;
                    bp.flags = 0;
                    if (ptrace(PTRACE_CLRBP, pid, (void *)0, &bp) == 0)
                        local_bp[i].enabled = 0;
                }
            }
            long rc = ptrace(PTRACE_DETACH, pid, (void *)0, (void *)0);
            if (rc < 0) {
                put_err("pdb: DETACH failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                return 1;
            }
            *child_stopped = 0;
            put_str("detached\n");
        }
        if (!attach_mode) {
            (void)kill(pid, 9);
            (void)waitpid(pid, (void *)0, 0);
        }
        *done = 1;
        return 1;
    }

    return 0;
}
