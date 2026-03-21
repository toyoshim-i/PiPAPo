#include "pdb_internal.h"
#include "pdb_util.h"

static int handle_break_cmd(pdb_dispatch_ctx_t *ctx, char **tok, int ntok) {
  pid_t pid = ctx->pid;
  int child_stopped = *ctx->child_stopped;
  struct ppap_ptrace_caps *caps = ctx->caps;
  pdb_local_bp_t *local_bp = ctx->local_bp;
  struct ppap_ptrace_bp bp;
  uint32_t requested_flag = 0;
  long rc;

  if (!child_stopped) {
    put_err("pdb: child is not stopped\n");
    return 1;
  }
  if (ntok == 2) {
    if (!parse_u32(tok[1], &bp.addr)) {
      put_err("pdb: usage: break <addr>\n");
      return 1;
    }
  } else if (ntok == 3) {
    if (streq(tok[1], "sw")) {
      requested_flag = PPAP_PTRACE_BP_SW;
    } else if (streq(tok[1], "hw")) {
      requested_flag = PPAP_PTRACE_BP_HW;
    } else {
      put_err("pdb: usage: break <addr>\n");
      return 1;
    }
    if (!parse_u32(tok[2], &bp.addr)) {
      put_err("pdb: usage: break <addr>\n");
      return 1;
    }
  } else {
    put_err("pdb: usage: break <addr>\n");
    return 1;
  }
  bp.flags = requested_flag ? requested_flag : PPAP_PTRACE_BP_SW;
  if (ptrace(PTRACE_GETCAPS, pid, (void *)0, caps) == 0) {
    if (requested_flag == PPAP_PTRACE_BP_SW &&
        (caps->caps & PPAP_PTRACE_CAP_SW_BP) == 0u) {
      put_err("pdb: sw break not supported on this target/mapping\n");
      return 1;
    }
    if (requested_flag == PPAP_PTRACE_BP_HW &&
        (caps->caps & PPAP_PTRACE_CAP_HW_BP) == 0u) {
      put_err("pdb: hw break not supported on this target/mapping\n");
      return 1;
    }
    if (!requested_flag) {
      uint32_t cap_flag = select_bp_flag_from_caps(caps->caps);
      if (cap_flag == 0) {
        put_err("pdb: break not supported on this target/mapping\n");
        return 1;
      }
      bp.flags = cap_flag;
    }
  }
  bp.id = -1;
  rc = ptrace(PTRACE_SETBP, pid, (void *)0, &bp);
  if (rc < 0) {
    put_err("pdb: SETBP failed rc=");
    put_i32((int32_t)rc);
    put_chr('\n');
    return 1;
  }
  put_str("bp ");
  put_i32(bp.id);
  put_str(" @ ");
  put_hex32(bp.addr);
  put_chr('\n');
  if (bp.id >= 0 && bp.id < PDB_LOCAL_BP_MAX) {
    local_bp[bp.id].used = 1;
    local_bp[bp.id].enabled = 1;
    local_bp[bp.id].addr = bp.addr;
    local_bp[bp.id].flags = bp.flags;
  }
  return 1;
}

static int handle_disable_cmd(pdb_dispatch_ctx_t *ctx, char **tok, int ntok) {
  pid_t pid = ctx->pid;
  int child_stopped = *ctx->child_stopped;
  pdb_local_bp_t *local_bp = ctx->local_bp;
  struct ppap_ptrace_bp bp;
  uint32_t id = 0;
  long rc;

  if (!child_stopped) {
    put_err("pdb: child is not stopped\n");
    return 1;
  }
  if (ntok != 2 || !parse_u32(tok[1], &id)) {
    put_err("pdb: usage: disable <id>\n");
    return 1;
  }
  if (id >= PDB_LOCAL_BP_MAX || !local_bp[id].used) {
    put_err("pdb: unknown breakpoint id\n");
    return 1;
  }
  if (!local_bp[id].enabled) {
    put_str("bp ");
    put_u32(id);
    put_str(" already disabled\n");
    return 1;
  }
  bp.id = (int32_t)id;
  bp.addr = 0;
  bp.flags = 0;
  rc = ptrace(PTRACE_CLRBP, pid, (void *)0, &bp);
  if (rc < 0) {
    put_err("pdb: CLRBP failed rc=");
    put_i32((int32_t)rc);
    put_chr('\n');
    return 1;
  }
  local_bp[id].enabled = 0;
  put_str("bp ");
  put_u32(id);
  put_str(" disabled\n");
  return 1;
}

static int handle_enable_cmd(pdb_dispatch_ctx_t *ctx, char **tok, int ntok) {
  pid_t pid = ctx->pid;
  int child_stopped = *ctx->child_stopped;
  struct ppap_ptrace_caps *caps = ctx->caps;
  pdb_local_bp_t *local_bp = ctx->local_bp;
  struct ppap_ptrace_bp bp;
  uint32_t id = 0;
  long rc;

  if (!child_stopped) {
    put_err("pdb: child is not stopped\n");
    return 1;
  }
  if (ntok != 2 || !parse_u32(tok[1], &id)) {
    put_err("pdb: usage: enable <id>\n");
    return 1;
  }
  if (id >= PDB_LOCAL_BP_MAX || !local_bp[id].used) {
    put_err("pdb: unknown breakpoint id\n");
    return 1;
  }
  if (local_bp[id].enabled) {
    put_str("bp ");
    put_u32(id);
    put_str(" already enabled\n");
    return 1;
  }

  bp.id = -1;
  bp.addr = local_bp[id].addr;
  bp.flags = local_bp[id].flags;
  if (bp.flags == 0) {
    bp.flags = PPAP_PTRACE_BP_SW;
    if (ptrace(PTRACE_GETCAPS, pid, (void *)0, caps) == 0) {
      uint32_t cap_flag = select_bp_flag_from_caps(caps->caps);
      if (cap_flag == 0) {
        put_err("pdb: break not supported on this target/mapping\n");
        return 1;
      }
      bp.flags = cap_flag;
    }
  }
  rc = ptrace(PTRACE_SETBP, pid, (void *)0, &bp);
  if (rc < 0) {
    put_err("pdb: SETBP failed rc=");
    put_i32((int32_t)rc);
    put_chr('\n');
    return 1;
  }
  if (bp.id != (int32_t)id) {
    if (bp.id < 0 || bp.id >= PDB_LOCAL_BP_MAX || local_bp[bp.id].used) {
      struct ppap_ptrace_bp rollback;
      rollback.id = bp.id;
      rollback.addr = 0;
      rollback.flags = 0;
      (void)ptrace(PTRACE_CLRBP, pid, (void *)0, &rollback);
      put_err("pdb: enable remap failed\n");
      return 1;
    }
    local_bp[bp.id].used = local_bp[id].used;
    local_bp[bp.id].enabled = 1;
    local_bp[bp.id].addr = local_bp[id].addr;
    local_bp[bp.id].flags = bp.flags;
    local_bp[id].used = 0;
    local_bp[id].enabled = 0;
    local_bp[id].addr = 0;
    local_bp[id].flags = 0;
    put_str("bp ");
    put_u32(id);
    put_str(" enabled as ");
    put_i32(bp.id);
    put_chr('\n');
    return 1;
  }
  local_bp[id].enabled = 1;
  local_bp[id].flags = bp.flags;
  put_str("bp ");
  put_u32(id);
  put_str(" enabled\n");
  return 1;
}

static int handle_delete_cmd(pdb_dispatch_ctx_t *ctx, char **tok, int ntok) {
  pid_t pid = ctx->pid;
  int child_stopped = *ctx->child_stopped;
  pdb_local_bp_t *local_bp = ctx->local_bp;
  struct ppap_ptrace_bp bp;
  uint32_t id = 0;
  long rc;

  if (!child_stopped) {
    put_err("pdb: child is not stopped\n");
    return 1;
  }
  if (ntok != 2 || !parse_u32(tok[1], &id)) {
    put_err("pdb: usage: delete <id>\n");
    return 1;
  }
  if (id >= PDB_LOCAL_BP_MAX || !local_bp[id].used) {
    put_err("pdb: unknown breakpoint id\n");
    return 1;
  }
  if (local_bp[id].enabled) {
    bp.id = (int32_t)id;
    bp.addr = 0;
    bp.flags = 0;
    rc = ptrace(PTRACE_CLRBP, pid, (void *)0, &bp);
    if (rc < 0) {
      put_err("pdb: CLRBP failed rc=");
      put_i32((int32_t)rc);
      put_chr('\n');
      return 1;
    }
  }
  put_str("bp ");
  put_u32(id);
  put_str(" cleared\n");
  if (id < PDB_LOCAL_BP_MAX) {
    local_bp[id].used = 0;
    local_bp[id].enabled = 0;
    local_bp[id].addr = 0;
    local_bp[id].flags = 0;
  }
  return 1;
}

static int handle_info_break_cmd(pdb_dispatch_ctx_t *ctx, char **tok,
                                 int ntok) {
  pdb_local_bp_t *local_bp = ctx->local_bp;

  if (ntok == 2 && (streq(tok[1], "break") || streq(tok[1], "b"))) {
    int found = 0;
    for (int i = 0; i < PDB_LOCAL_BP_MAX; i++) {
      if (!local_bp[i].used) continue;
      found = 1;
      put_str("bp ");
      put_u32((uint32_t)i);
      put_str(" @ ");
      put_hex32(local_bp[i].addr);
      put_str(" ");
      if (local_bp[i].enabled)
        put_str("enabled");
      else
        put_str("disabled");
      put_str(" ");
      put_str(bp_flag_name(local_bp[i].flags));
      put_chr('\n');
    }
    if (!found) put_str("no breakpoints\n");
    return 1;
  }
  put_err("pdb: usage: info break\n");
  return 1;
}

int handle_breakpoint_commands(pdb_dispatch_ctx_t *ctx, char **tok, int ntok) {
  if (streq(tok[0], "break") || streq(tok[0], "b"))
    return handle_break_cmd(ctx, tok, ntok);
  if (streq(tok[0], "disable")) return handle_disable_cmd(ctx, tok, ntok);
  if (streq(tok[0], "enable")) return handle_enable_cmd(ctx, tok, ntok);
  if (streq(tok[0], "delete") || streq(tok[0], "d"))
    return handle_delete_cmd(ctx, tok, ntok);
  if (streq(tok[0], "info")) return handle_info_break_cmd(ctx, tok, ntok);
  return 0;
}
