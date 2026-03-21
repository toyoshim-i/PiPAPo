#include "pdb_internal.h"
#include "pdb_trace_util.h"
#include "pdb_util.h"

static char pdb_script_storage[PDB_SCRIPT_BUF_MAX];

/* Returns 1 when caller should return with main_exit_code. */
static int dispatch_command(pdb_dispatch_ctx_t *ctx, char **tok, int ntok)
{
    int run_cmd;

    if (handle_inspect_commands(ctx, tok, ntok))
        return 0;

    run_cmd = handle_run_control_commands(ctx, tok, ntok);
    if (run_cmd == 2)
        return 1;
    if (run_cmd == 1)
        return 0;

    if (handle_write_commands(ctx, tok, ntok))
        return 0;

    if (handle_breakpoint_commands(ctx, tok, ntok))
        return 0;

    if (handle_session_commands(ctx, tok, ntok))
        return 0;

    put_err("pdb: unknown command\n");
    return 0;
}

int main(int argc, char *argv[])
{
    int argi = 1;
    int show_prompt = 1;
    int batch_mode = 0;
    int scripted_mode = 0;
    int attach_mode = 0;
    char *script_cmds[PDB_SCRIPT_CMD_MAX];
    int script_storage_used = 0;
    int script_count = 0;
    int script_index = 0;
    pid_t attach_pid = 0;
    pid_t pid;
    int child_stopped = 0;
    int child_exit_code = 0;
    int done = 0;
    int exit_code = 1;
    char line[PDB_SCRIPT_LINE_MAX];
    char *tok[8];
    struct ppap_ptrace_event last_ev;
    struct ppap_ptrace_caps caps;
    pdb_local_bp_t local_bp[PDB_LOCAL_BP_MAX];
    pdb_dispatch_ctx_t ctx;

    if (!parse_startup_options(argc, argv, &argi, &show_prompt, &batch_mode,
                               &scripted_mode, &attach_mode, &attach_pid,
                               script_cmds, &script_count, pdb_script_storage,
                               &script_storage_used, &exit_code))
        return exit_code;

    if (!validate_startup_options(argc, argi, attach_mode, scripted_mode,
                                  script_count))
        return 1;

    init_local_bp_table(local_bp);

    if (!start_tracee(argv, argi, attach_mode, attach_pid, &pid))
        return 1;

    {
        int wr = wait_child(pid, &child_stopped, &child_exit_code, &last_ev,
                            !batch_mode);
        if (wr < 0)
            return 1;
        if (wr > 0)
            return child_exit_code;
    }

    if (!batch_mode && ptrace(PTRACE_GETCAPS, pid, (void *)0, &caps) == 0) {
        put_str("target ");
        print_caps(&caps);
    }

    ctx.pid = pid;
    ctx.attach_mode = attach_mode;
    ctx.child_stopped = &child_stopped;
    ctx.child_exit_code = &child_exit_code;
    ctx.batch_mode = batch_mode;
    ctx.done = &done;
    ctx.main_exit_code = &exit_code;
    ctx.last_ev = &last_ev;
    ctx.caps = &caps;
    ctx.local_bp = local_bp;

    while (!done) {
        if (!read_next_command_line(line, sizeof(line), script_cmds,
                                    script_count, &script_index, show_prompt))
            break;

        {
            int ntok = split_tokens(line, tok, 8);
            if (ntok <= 0)
                continue;
            if (dispatch_command(&ctx, tok, ntok))
                return exit_code;
        }
    }

    /* Cleanup: if the tracee is still alive (e.g., scripted commands exhausted
     * without explicit quit), detach and reap it to avoid zombie leaks. */
    if (!attach_mode) {
        if (child_stopped)
            (void)ptrace(PTRACE_DETACH, pid, (void *)0, (void *)0);
        (void)kill(pid, 9);
        (void)waitpid(pid, (void *)0, 0);
    }

    return 0;
}
