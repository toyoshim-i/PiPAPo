#include "pdb_internal.h"
#include "pdb_util.h"

static const char *event_name(uint32_t ev) {
  switch (ev) {
    case PPAP_TRACE_EVENT_NONE:
      return "none";
    case PPAP_TRACE_EVENT_EXEC:
      return "exec";
    case PPAP_TRACE_EVENT_SYSCALL_ENTER:
      return "sys-enter";
    case PPAP_TRACE_EVENT_SYSCALL_EXIT:
      return "sys-exit";
    case PPAP_TRACE_EVENT_SUBSYS_ENTER:
      return "subsys-enter";
    case PPAP_TRACE_EVENT_SUBSYS_EXIT:
      return "subsys-exit";
    case PPAP_TRACE_EVENT_DEBUG_STOP:
      return "debug-stop";
    default:
      return "event";
  }
}

const char *abi_name(uint32_t abi) {
  switch (abi) {
    case PPAP_TRACE_ABI_PPAP:
      return "ppap";
    case PPAP_TRACE_ABI_H68K_DOS:
      return "h68k-dos";
    case PPAP_TRACE_ABI_H68K_IOCS:
      return "h68k-iocs";
    case PPAP_TRACE_ABI_CPM_BDOS:
      return "cpm-bdos";
    case PPAP_TRACE_ABI_CPM_BIOS:
      return "cpm-bios";
    case PPAP_TRACE_ABI_SOS:
      return "sos";
    default:
      return "unknown";
  }
}

const char *regset_name(uint32_t regset) {
  switch (regset) {
    case PPAP_TRACE_REGSET_ARM:
      return "arm";
    case PPAP_TRACE_REGSET_M68K:
      return "m68k";
    case PPAP_TRACE_REGSET_Z80:
      return "z80";
    default:
      return "unknown";
  }
}

const char *surface_name_for_regset(uint32_t regset) {
  if (regset == PPAP_TRACE_REGSET_Z80) return "ecpu";
  if (regset == PPAP_TRACE_REGSET_NONE) return "unknown";
  return "real";
}

const char *surface_name_for_value(uint32_t surface) {
  switch (surface) {
    case PPAP_TRACE_SURFACE_REAL:
      return "real";
    case PPAP_TRACE_SURFACE_ECPU:
      return "ecpu";
    default:
      return "unknown";
  }
}

static void print_surface_mask(uint32_t surfaces) {
  int has = 0;
  if (surfaces & PPAP_PTRACE_SURFACE_MASK_REAL) {
    put_str("real");
    has = 1;
  }
  if (surfaces & PPAP_PTRACE_SURFACE_MASK_ECPU) {
    if (has) put_chr('|');
    put_str("ecpu");
    has = 1;
  }
  if (!has) put_str("none");
}

static void print_caps_bits(uint32_t caps_bits) {
  int has = 0;

  if (caps_bits & PPAP_PTRACE_CAP_GETREGS) {
    put_str("getregs");
    has = 1;
  }
  if (caps_bits & PPAP_PTRACE_CAP_SETREGS) {
    put_str(has ? "|setregs" : "setregs");
    has = 1;
  }
  if (caps_bits & PPAP_PTRACE_CAP_PEEKPOKE) {
    put_str(has ? "|peekpoke" : "peekpoke");
    has = 1;
  }
  if (caps_bits & PPAP_PTRACE_CAP_SINGLESTEP) {
    put_str(has ? "|step" : "step");
    has = 1;
  }
  if (caps_bits & PPAP_PTRACE_CAP_SW_BP) {
    put_str(has ? "|sw-bp" : "sw-bp");
    has = 1;
  }
  if (caps_bits & PPAP_PTRACE_CAP_HW_BP) {
    put_str(has ? "|hw-bp" : "hw-bp");
    has = 1;
  }
  if (!has) put_str("none");
}

void print_event(const struct ppap_ptrace_event *ev) {
  put_str("stop ");
  put_str(event_name(ev->event));
  put_str(" abi=");
  put_str(abi_name(ev->abi));

  if (ev->event == PPAP_TRACE_EVENT_DEBUG_STOP) {
    int has_reason = 0;
    put_str(" pc=");
    put_hex32(ev->args[0]);
    put_str(" flags=");
    put_hex32(ev->flags);
    put_str(" reason=");
    if (ev->flags & PPAP_DEBUG_STOP_STEP) {
      put_str("step");
      has_reason = 1;
    }
    if (ev->flags & PPAP_DEBUG_STOP_SW_BP) {
      if (has_reason) put_chr('|');
      put_str("sw-bp");
      has_reason = 1;
    }
    if (ev->flags & PPAP_DEBUG_STOP_HW_BP) {
      if (has_reason) put_chr('|');
      put_str("hw-bp");
      has_reason = 1;
    }
    if (!has_reason) put_str("unknown");
  } else {
    put_str(" nr=");
    put_u32(ev->nr);
    put_str(" ret=");
    put_i32(ev->ret);
  }
  put_chr('\n');
}

void print_caps(const struct ppap_ptrace_caps *caps) {
  put_str("regset=");
  put_str(regset_name(caps->regset));
  put_str(" abi=");
  put_str(abi_name(caps->abi));
  put_str(" surface=");
  put_str(surface_name_for_value(caps->surface));
  put_str(" surfaces=");
  print_surface_mask(caps->surfaces);
  put_str(" caps=");
  put_hex32(caps->caps);
  put_str(" [");
  print_caps_bits(caps->caps);
  put_chr(']');
  put_str(" max_bps=");
  put_u32(caps->max_bps);
  put_chr('\n');
}

static uint32_t reg_count(uint32_t regset) {
  switch (regset) {
    case PPAP_TRACE_REGSET_ARM:
      return 17;
    case PPAP_TRACE_REGSET_M68K:
      return 20;
    case PPAP_TRACE_REGSET_Z80:
      return 18;
    default:
      return 0;
  }
}

int reg_is16(uint32_t regset) { return regset == PPAP_TRACE_REGSET_Z80; }

const char *reg_name(uint32_t regset, uint32_t idx) {
  if (regset == PPAP_TRACE_REGSET_ARM) {
    switch (idx) {
      case 0:
        return "r0";
      case 1:
        return "r1";
      case 2:
        return "r2";
      case 3:
        return "r3";
      case 4:
        return "r4";
      case 5:
        return "r5";
      case 6:
        return "r6";
      case 7:
        return "r7";
      case 8:
        return "r8";
      case 9:
        return "r9";
      case 10:
        return "r10";
      case 11:
        return "r11";
      case 12:
        return "r12";
      case 13:
        return "sp";
      case 14:
        return "lr";
      case 15:
        return "pc";
      case 16:
        return "xpsr";
      default:
        return (const char *)0;
    }
  } else if (regset == PPAP_TRACE_REGSET_M68K) {
    switch (idx) {
      case 0:
        return "d0";
      case 1:
        return "d1";
      case 2:
        return "d2";
      case 3:
        return "d3";
      case 4:
        return "d4";
      case 5:
        return "d5";
      case 6:
        return "d6";
      case 7:
        return "d7";
      case 8:
        return "a0";
      case 9:
        return "a1";
      case 10:
        return "a2";
      case 11:
        return "a3";
      case 12:
        return "a4";
      case 13:
        return "a5";
      case 14:
        return "a6";
      case 15:
        return "a7";
      case 16:
        return "pc";
      case 17:
        return "sr";
      case 18:
        return "usp";
      case 19:
        return "ksp";
      default:
        return (const char *)0;
    }
  } else if (regset == PPAP_TRACE_REGSET_Z80) {
    switch (idx) {
      case 0:
        return "af";
      case 1:
        return "bc";
      case 2:
        return "de";
      case 3:
        return "hl";
      case 4:
        return "ix";
      case 5:
        return "iy";
      case 6:
        return "sp";
      case 7:
        return "pc";
      case 8:
        return "af2";
      case 9:
        return "bc2";
      case 10:
        return "de2";
      case 11:
        return "hl2";
      case 12:
        return "iff1";
      case 13:
        return "iff2";
      case 14:
        return "im";
      case 15:
        return "wz";
      case 16:
        return "i";
      case 17:
        return "r";
      default:
        return (const char *)0;
    }
  }
  return (const char *)0;
}

int regset_pc_sp_indices(uint32_t regset, uint32_t *pc_idx, uint32_t *sp_idx) {
  switch (regset) {
    case PPAP_TRACE_REGSET_ARM:
      *pc_idx = 15u;
      *sp_idx = 13u;
      return 1;
    case PPAP_TRACE_REGSET_M68K:
      *pc_idx = 16u;
      *sp_idx = 15u;
      return 1;
    case PPAP_TRACE_REGSET_Z80:
      *pc_idx = 7u;
      *sp_idx = 6u;
      return 1;
    default:
      return 0;
  }
}

int regset_pc_index(uint32_t regset, uint32_t *pc_idx) {
  uint32_t sp_idx = 0;
  return regset_pc_sp_indices(regset, pc_idx, &sp_idx);
}

void print_reg_value(uint32_t regset, uint32_t value) {
  if (reg_is16(regset))
    put_hex16(value);
  else
    put_hex32(value);
}

int reg_index_from_token(const struct ppap_ptrace_regs *regs, const char *tok,
                         uint32_t *idx_out) {
  uint32_t idx = 0;
  uint32_t count = reg_count(regs->regset);

  if (parse_u32(tok, &idx)) {
    if (idx >= regs->words) return 0;
    *idx_out = idx;
    return 1;
  }

  if (count > regs->words) count = regs->words;
  for (idx = 0; idx < count; idx++) {
    const char *name = reg_name(regs->regset, idx);
    if (name && streq(tok, name)) {
      *idx_out = idx;
      return 1;
    }
  }
  return 0;
}

void print_regs(const struct ppap_ptrace_regs *regs) {
  int is16 = reg_is16(regs->regset);
  uint32_t count = reg_count(regs->regset);

  put_str("regset=");
  put_str(regset_name(regs->regset));
  put_str(" abi=");
  put_str(abi_name(regs->abi));
  put_str(" words=");
  put_u32(regs->words);
  put_chr('\n');

  if (count > regs->words) count = regs->words;
  if (count > PPAP_PTRACE_REGS_MAX) count = PPAP_PTRACE_REGS_MAX;
  for (uint32_t i = 0; i < count; i++) {
    const char *name = reg_name(regs->regset, i);
    if (name) {
      put_str(name);
    } else {
      put_str("r");
      put_u32(i);
    }
    put_str("=");
    if (is16)
      put_hex16(regs->regs[i]);
    else
      put_hex32(regs->regs[i]);
    put_chr('\n');
  }
}
