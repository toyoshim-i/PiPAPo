#ifndef PPAP_USER_PDB_INTERNAL_H
#define PPAP_USER_PDB_INTERNAL_H

#include "syscall.h"

#define PDB_LOCAL_BP_MAX 32
#define PDB_SCRIPT_CMD_MAX 32
#define PDB_SCRIPT_LINE_MAX 128
#define PDB_SCRIPT_BUF_MAX 2048

typedef struct {
  uint8_t used;
  uint8_t enabled;
  uint32_t addr;
  uint32_t flags;
} pdb_local_bp_t;

typedef struct {
  pid_t pid;
  int attach_mode;
  int *child_stopped;
  int *child_exit_code;
  int batch_mode;
  int *done;
  int *main_exit_code;
  struct ppap_ptrace_event *last_ev;
  struct ppap_ptrace_caps *caps;
  pdb_local_bp_t *local_bp;
} pdb_dispatch_ctx_t;

/* Register / event formatting and mapping helpers. */
const char *abi_name(uint32_t abi);
const char *regset_name(uint32_t regset);
const char *surface_name_for_regset(uint32_t regset);
const char *surface_name_for_value(uint32_t surface);
void print_event(const struct ppap_ptrace_event *ev);
void print_caps(const struct ppap_ptrace_caps *caps);
int reg_is16(uint32_t regset);
const char *reg_name(uint32_t regset, uint32_t idx);
int regset_pc_sp_indices(uint32_t regset, uint32_t *pc_idx, uint32_t *sp_idx);
int regset_pc_index(uint32_t regset, uint32_t *pc_idx);
void print_reg_value(uint32_t regset, uint32_t value);
int reg_index_from_token(const struct ppap_ptrace_regs *regs, const char *tok,
                         uint32_t *idx_out);
void print_regs(const struct ppap_ptrace_regs *regs);

/* Tracee lifecycle / state helpers. */
int wait_child(pid_t pid, int *stopped, int *exit_code,
               struct ppap_ptrace_event *ev, int report_event);
int ensure_child_stopped(int child_stopped);
int get_regs_if_stopped(pid_t pid, int child_stopped,
                        struct ppap_ptrace_regs *regs);
int get_caps_if_stopped(pid_t pid, int child_stopped,
                        struct ppap_ptrace_caps *caps);
void init_local_bp_table(pdb_local_bp_t *local_bp);
int start_tracee(char *argv[], int argi, int attach_mode, pid_t attach_pid,
                 pid_t *pid_out);
int handle_run_control_commands(pdb_dispatch_ctx_t *ctx, char **tok, int ntok);
int handle_session_commands(pdb_dispatch_ctx_t *ctx, char **tok, int ntok);

/* CLI setup and line reading. */
void usage(void);
void print_help(void);
int parse_startup_options(int argc, char *argv[], int *argi, int *show_prompt,
                          int *batch_mode, int *scripted_mode, int *attach_mode,
                          pid_t *attach_pid, char **script_cmds,
                          int *script_count, char *script_storage,
                          int *script_storage_used, int *exit_code);
int validate_startup_options(int argc, int argi, int attach_mode,
                             int scripted_mode, int script_count);
int read_next_command_line(char *line, int line_size, char **script_cmds,
                           int script_count, int *script_index,
                           int show_prompt);

/* Command handlers. */
int handle_inspect_commands(pdb_dispatch_ctx_t *ctx, char **tok, int ntok);
int handle_write_commands(pdb_dispatch_ctx_t *ctx, char **tok, int ntok);
int handle_breakpoint_commands(pdb_dispatch_ctx_t *ctx, char **tok, int ntok);

#endif /* PPAP_USER_PDB_INTERNAL_H */
