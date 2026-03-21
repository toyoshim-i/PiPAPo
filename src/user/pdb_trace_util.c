#include "pdb_trace_util.h"

#include "pdb_util.h"
#include "syscall.h"

void print_mem_words(pid_t pid, uint32_t addr, uint32_t count) {
  if (count == 0) count = 1;
  if (count > 64) count = 64;

  for (uint32_t i = 0; i < count; i++) {
    uint32_t word = 0;
    long rc = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)addr, &word);
    if (rc < 0) {
      put_err("pdb: PEEKDATA failed rc=");
      put_i32((int32_t)rc);
      put_chr('\n');
      return;
    }
    put_hex32(addr);
    put_str(": ");
    put_hex32(word);
    put_chr('\n');
    addr += 4;
  }
}

void print_mem_bytes(pid_t pid, uint32_t addr, uint32_t count) {
  if (count == 0) count = 1;
  if (count > 256) count = 256;

  for (uint32_t i = 0; i < count; i++) {
    uint8_t b = 0;
    int rc = peek_u8(pid, addr, &b);
    if (rc < 0) {
      put_err("pdb: PEEKDATA failed rc=");
      put_i32((int32_t)rc);
      put_chr('\n');
      return;
    }
    put_hex32(addr);
    put_str(": ");
    put_hex8((uint32_t)b);
    put_chr('\n');
    addr += 1u;
  }
}

void print_mem_halfwords(pid_t pid, uint32_t addr, uint32_t count) {
  if (count == 0) count = 1;
  if (count > 128) count = 128;

  for (uint32_t i = 0; i < count; i++) {
    uint16_t value = 0;
    int rc = peek_u16le(pid, addr, &value);
    if (rc < 0) {
      put_err("pdb: PEEKDATA failed rc=");
      put_i32((int32_t)rc);
      put_chr('\n');
      return;
    }
    put_hex32(addr);
    put_str(": ");
    put_hex16((uint32_t)value);
    put_chr('\n');
    addr += 2u;
  }
}

static int peek_word(pid_t pid, uint32_t addr, uint32_t *word_out) {
  uint32_t base = addr & ~3u;
  long rc = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)base, word_out);
  if (rc < 0) return (int)rc;
  return 0;
}

int peek_u8(pid_t pid, uint32_t addr, uint8_t *byte_out) {
  uint32_t word = 0;
  int rc = peek_word(pid, addr, &word);
  if (rc < 0) return rc;
  *byte_out = (uint8_t)((word >> ((addr & 3u) * 8u)) & 0xffu);
  return 0;
}

int peek_u16le(pid_t pid, uint32_t addr, uint16_t *value_out) {
  uint8_t lo = 0;
  uint8_t hi = 0;
  int rc = peek_u8(pid, addr, &lo);
  if (rc < 0) return rc;
  rc = peek_u8(pid, addr + 1u, &hi);
  if (rc < 0) return rc;
  *value_out = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
  return 0;
}

int peek_u16be(pid_t pid, uint32_t addr, uint16_t *value_out) {
  uint8_t hi = 0;
  uint8_t lo = 0;
  int rc = peek_u8(pid, addr, &hi);
  if (rc < 0) return rc;
  rc = peek_u8(pid, addr + 1u, &lo);
  if (rc < 0) return rc;
  *value_out = (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
  return 0;
}

int peek_u32be(pid_t pid, uint32_t addr, uint32_t *value_out) {
  uint16_t hi = 0;
  uint16_t lo = 0;
  int rc = peek_u16be(pid, addr, &hi);
  if (rc < 0) return rc;
  rc = peek_u16be(pid, addr + 2u, &lo);
  if (rc < 0) return rc;
  *value_out = ((uint32_t)hi << 16) | (uint32_t)lo;
  return 0;
}

int peek_u32le(pid_t pid, uint32_t addr, uint32_t *value_out) {
  uint16_t lo = 0;
  uint16_t hi = 0;
  int rc = peek_u16le(pid, addr, &lo);
  if (rc < 0) return rc;
  rc = peek_u16le(pid, addr + 2u, &hi);
  if (rc < 0) return rc;
  *value_out = ((uint32_t)hi << 16) | (uint32_t)lo;
  return 0;
}

int poke_u8(pid_t pid, uint32_t addr, uint8_t byte_value) {
  uint32_t base = addr & ~3u;
  uint32_t old_word = 0;
  uint32_t new_word = 0;
  uint32_t shift = (addr & 3u) * 8u;
  long rc = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)base, &old_word);
  if (rc < 0) return (int)rc;
  new_word = (old_word & ~(0xffu << shift)) | ((uint32_t)byte_value << shift);
  rc = ptrace(PTRACE_POKEDATA, pid, (void *)(uintptr_t)base, &new_word);
  if (rc < 0) return (int)rc;
  return 0;
}

static int z80_disas_one(pid_t pid, uint32_t pc, uint32_t *next_pc) {
  uint8_t op = 0;
  uint8_t imm8 = 0;
  uint16_t imm16 = 0;
  uint32_t len = 1;
  int rc = peek_u8(pid, pc, &op);
  if (rc < 0) return rc;

  put_hex32(pc);
  put_str(": ");
  switch (op) {
    case 0x00:
      put_str("nop");
      len = 1;
      break;
    case 0x06:
      rc = peek_u8(pid, pc + 1u, &imm8);
      if (rc < 0) return rc;
      put_str("ld b,#");
      put_hex8(imm8);
      len = 2;
      break;
    case 0x0e:
      rc = peek_u8(pid, pc + 1u, &imm8);
      if (rc < 0) return rc;
      put_str("ld c,#");
      put_hex8(imm8);
      len = 2;
      break;
    case 0x16:
      rc = peek_u8(pid, pc + 1u, &imm8);
      if (rc < 0) return rc;
      put_str("ld d,#");
      put_hex8(imm8);
      len = 2;
      break;
    case 0x1e:
      rc = peek_u8(pid, pc + 1u, &imm8);
      if (rc < 0) return rc;
      put_str("ld e,#");
      put_hex8(imm8);
      len = 2;
      break;
    case 0x26:
      rc = peek_u8(pid, pc + 1u, &imm8);
      if (rc < 0) return rc;
      put_str("ld h,#");
      put_hex8(imm8);
      len = 2;
      break;
    case 0x2e:
      rc = peek_u8(pid, pc + 1u, &imm8);
      if (rc < 0) return rc;
      put_str("ld l,#");
      put_hex8(imm8);
      len = 2;
      break;
    case 0x3e:
      rc = peek_u8(pid, pc + 1u, &imm8);
      if (rc < 0) return rc;
      put_str("ld a,#");
      put_hex8(imm8);
      len = 2;
      break;
    case 0xc3:
      rc = peek_u16le(pid, pc + 1u, &imm16);
      if (rc < 0) return rc;
      put_str("jp ");
      put_hex16(imm16);
      len = 3;
      break;
    case 0xc9:
      put_str("ret");
      len = 1;
      break;
    case 0xcd:
      rc = peek_u16le(pid, pc + 1u, &imm16);
      if (rc < 0) return rc;
      put_str("call ");
      put_hex16(imm16);
      len = 3;
      break;
    case 0x76:
      put_str("halt");
      len = 1;
      break;
    default:
      put_str("db ");
      put_hex8(op);
      len = 1;
      break;
  }

  put_str(" ;");
  for (uint32_t i = 0; i < len; i++) {
    uint8_t b = 0;
    rc = peek_u8(pid, pc + i, &b);
    if (rc < 0) return rc;
    put_chr(' ');
    put_hex8(b);
  }
  put_chr('\n');
  *next_pc = pc + len;
  return 0;
}

void disas_z80(pid_t pid, uint32_t pc, uint32_t count) {
  if (count == 0) count = 8;
  if (count > 64) count = 64;

  for (uint32_t i = 0; i < count; i++) {
    int rc = z80_disas_one(pid, pc, &pc);
    if (rc < 0) {
      put_err("pdb: disas failed rc=");
      put_i32((int32_t)rc);
      put_chr('\n');
      return;
    }
  }
}

static const char *m68k_cc_name(uint16_t cc) {
  switch (cc & 0x0f) {
    case 0x0:
      return "bra";
    case 0x1:
      return "bsr";
    case 0x2:
      return "bhi";
    case 0x3:
      return "bls";
    case 0x4:
      return "bcc";
    case 0x5:
      return "bcs";
    case 0x6:
      return "bne";
    case 0x7:
      return "beq";
    case 0x8:
      return "bvc";
    case 0x9:
      return "bvs";
    case 0xa:
      return "bpl";
    case 0xb:
      return "bmi";
    case 0xc:
      return "bge";
    case 0xd:
      return "blt";
    case 0xe:
      return "bgt";
    default:
      return "ble";
  }
}

static int m68k_disas_one(pid_t pid, uint32_t pc, uint32_t *next_pc) {
  uint16_t op = 0;
  uint16_t imm16 = 0;
  uint32_t imm32 = 0;
  uint32_t len = 2;
  int rc = peek_u16be(pid, pc, &op);
  if (rc < 0) return rc;

  put_hex32(pc);
  put_str(": ");

  if (op == 0x4e71) {
    put_str("nop");
    len = 2;
  } else if (op == 0x4e75) {
    put_str("rts");
    len = 2;
  } else if (op == 0x4e73) {
    put_str("rte");
    len = 2;
  } else if (op == 0x4e72) {
    rc = peek_u16be(pid, pc + 2u, &imm16);
    if (rc < 0) return rc;
    put_str("stop #");
    put_hex16(imm16);
    len = 4;
  } else if ((op & 0xfff0u) == 0x4e40u) {
    put_str("trap #");
    put_u32((uint32_t)(op & 0x000fu));
    len = 2;
  } else if (op == 0x4eb9u || op == 0x4ef9u) {
    rc = peek_u32be(pid, pc + 2u, &imm32);
    if (rc < 0) return rc;
    if (op == 0x4eb9u)
      put_str("jsr ");
    else
      put_str("jmp ");
    put_hex32(imm32);
    len = 6;
  } else if ((op & 0xf100u) == 0x7000u) {
    int32_t imm8 = (int32_t)(int8_t)(op & 0x00ffu);
    uint32_t reg = (uint32_t)((op >> 9) & 0x7u);
    put_str("moveq #");
    put_i32(imm8);
    put_str(",d");
    put_u32(reg);
    len = 2;
  } else if ((op & 0xf000u) == 0x6000u) {
    uint32_t target = 0;
    int32_t disp = 0;
    put_str(m68k_cc_name((uint16_t)((op >> 8) & 0x0fu)));
    put_chr(' ');
    disp = (int8_t)(op & 0x00ffu);
    if ((op & 0x00ffu) == 0x00u) {
      rc = peek_u16be(pid, pc + 2u, &imm16);
      if (rc < 0) return rc;
      disp = (int16_t)imm16;
      len = 4;
    } else {
      len = 2;
    }
    target = pc + len + (uint32_t)disp;
    put_hex32(target);
  } else {
    put_str("dc.w ");
    put_hex16(op);
    len = 2;
  }

  put_str(" ;");
  for (uint32_t i = 0; i < len; i++) {
    uint8_t b = 0;
    rc = peek_u8(pid, pc + i, &b);
    if (rc < 0) return rc;
    put_chr(' ');
    put_hex8(b);
  }
  put_chr('\n');
  *next_pc = pc + len;
  return 0;
}

void disas_m68k(pid_t pid, uint32_t pc, uint32_t count) {
  if (count == 0) count = 8;
  if (count > 64) count = 64;

  for (uint32_t i = 0; i < count; i++) {
    int rc = m68k_disas_one(pid, pc, &pc);
    if (rc < 0) {
      put_err("pdb: disas failed rc=");
      put_i32((int32_t)rc);
      put_chr('\n');
      return;
    }
  }
}

static int thumb_disas_one(pid_t pid, uint32_t pc, uint32_t *next_pc) {
  uint16_t op = 0;
  uint32_t fetch_pc = pc & ~1u;
  uint32_t len = 2;
  int rc = peek_u16le(pid, fetch_pc, &op);
  if (rc < 0) return rc;

  put_hex32(fetch_pc);
  put_str(": ");

  if (op == 0xbf00u) {
    put_str("nop");
    len = 2;
  } else if (op == 0x4770u) {
    put_str("bx lr");
    len = 2;
  } else if ((op & 0xf800u) == 0x2000u) {
    uint32_t rd = (op >> 8) & 0x7u;
    uint32_t imm8 = op & 0xffu;
    put_str("movs r");
    put_u32(rd);
    put_str(",#");
    put_u32(imm8);
    len = 2;
  } else if ((op & 0xf800u) == 0x3000u) {
    uint32_t rd = (op >> 8) & 0x7u;
    uint32_t imm8 = op & 0xffu;
    put_str("adds r");
    put_u32(rd);
    put_str(",#");
    put_u32(imm8);
    len = 2;
  } else if ((op & 0xf800u) == 0x3800u) {
    uint32_t rd = (op >> 8) & 0x7u;
    uint32_t imm8 = op & 0xffu;
    put_str("subs r");
    put_u32(rd);
    put_str(",#");
    put_u32(imm8);
    len = 2;
  } else if ((op & 0xf800u) == 0xe000u) {
    int32_t disp = (int32_t)(op & 0x07ffu);
    uint32_t target = 0;
    if (disp & 0x0400u) disp |= ~0x07ff;
    disp <<= 1;
    target = fetch_pc + 4u + (uint32_t)disp;
    put_str("b ");
    put_hex32(target);
    len = 2;
  } else if ((op & 0xff00u) == 0xdf00u) {
    put_str("svc #");
    put_u32((uint32_t)(op & 0x00ffu));
    len = 2;
  } else {
    put_str("hword ");
    put_hex16(op);
    len = 2;
  }

  put_str(" ;");
  for (uint32_t i = 0; i < len; i++) {
    uint8_t b = 0;
    rc = peek_u8(pid, fetch_pc + i, &b);
    if (rc < 0) return rc;
    put_chr(' ');
    put_hex8(b);
  }
  put_chr('\n');
  *next_pc = fetch_pc + len;
  return 0;
}

void disas_thumb(pid_t pid, uint32_t pc, uint32_t count) {
  if (count == 0) count = 8;
  if (count > 64) count = 64;

  for (uint32_t i = 0; i < count; i++) {
    int rc = thumb_disas_one(pid, pc, &pc);
    if (rc < 0) {
      put_err("pdb: disas failed rc=");
      put_i32((int32_t)rc);
      put_chr('\n');
      return;
    }
  }
}

int z80_is_call_opcode(uint8_t op) {
  switch (op) {
    case 0xc4:
    case 0xcc:
    case 0xcd:
    case 0xd4:
    case 0xdc:
    case 0xe4:
    case 0xec:
    case 0xf4:
    case 0xfc:
      return 1;
    default:
      return 0;
  }
}

static void print_bt_frame_u32(uint32_t frame, uint32_t pc, uint32_t sp,
                               uint32_t fp) {
  put_chr('#');
  put_u32(frame);
  put_str(" pc=");
  put_hex32(pc);
  put_str(" sp=");
  put_hex32(sp);
  put_str(" fp=");
  put_hex32(fp);
  put_chr('\n');
}

static void print_bt_frame_u16(uint32_t frame, uint32_t pc, uint32_t sp) {
  put_chr('#');
  put_u32(frame);
  put_str(" pc=");
  put_hex16(pc);
  put_str(" sp=");
  put_hex16(sp);
  put_chr('\n');
}

void print_backtrace(pid_t pid, const struct ppap_ptrace_regs *regs,
                     uint32_t count) {
  if (count == 0) count = 1;
  if (count > 32) count = 32;

  if (regs->regset == PPAP_TRACE_REGSET_Z80) {
    if (regs->words <= 7) {
      put_err("pdb: z80 regset too small for bt\n");
      return;
    }
    print_bt_frame_u16(0, regs->regs[7], regs->regs[6]);
    put_str("bt: z80 frame unwinding is not supported\n");
    return;
  }

  if (regs->regset == PPAP_TRACE_REGSET_M68K) {
    uint32_t frame = 0;
    uint32_t pc = 0;
    uint32_t sp = 0;
    uint32_t fp = 0;
    if (regs->words <= 16) {
      put_err("pdb: m68k regset too small for bt\n");
      return;
    }
    pc = regs->regs[16];
    sp = regs->regs[15];
    fp = regs->regs[14];
    print_bt_frame_u32(frame, pc, sp, fp);
    while ((frame + 1u) < count && fp != 0u) {
      uint32_t next_fp = 0;
      uint32_t ret_pc = 0;
      int rc = peek_u32be(pid, fp, &next_fp);
      if (rc < 0) {
        put_err("pdb: bt read failed rc=");
        put_i32((int32_t)rc);
        put_chr('\n');
        return;
      }
      rc = peek_u32be(pid, fp + 4u, &ret_pc);
      if (rc < 0) {
        put_err("pdb: bt read failed rc=");
        put_i32((int32_t)rc);
        put_chr('\n');
        return;
      }
      if (next_fp <= fp || (next_fp & 1u)) break;
      frame++;
      pc = ret_pc;
      sp = fp + 8u;
      fp = next_fp;
      print_bt_frame_u32(frame, pc, sp, fp);
    }
    return;
  }

  if (regs->regset == PPAP_TRACE_REGSET_ARM) {
    uint32_t frame = 0;
    uint32_t pc = 0;
    uint32_t sp = 0;
    uint32_t fp = 0;
    if (regs->words <= 15) {
      put_err("pdb: arm regset too small for bt\n");
      return;
    }
    pc = regs->regs[15];
    sp = regs->regs[13];
    if (regs->words > 7 && regs->regs[7] != 0u)
      fp = regs->regs[7];
    else if (regs->words > 11)
      fp = regs->regs[11];
    print_bt_frame_u32(frame, pc, sp, fp);
    if (fp == 0u) {
      put_str("bt: frame pointer unavailable\n");
      return;
    }
    while ((frame + 1u) < count && fp != 0u) {
      uint32_t next_fp = 0;
      uint32_t ret_pc = 0;
      int rc = peek_u32le(pid, fp, &next_fp);
      if (rc < 0) {
        put_err("pdb: bt read failed rc=");
        put_i32((int32_t)rc);
        put_chr('\n');
        return;
      }
      rc = peek_u32le(pid, fp + 4u, &ret_pc);
      if (rc < 0) {
        put_err("pdb: bt read failed rc=");
        put_i32((int32_t)rc);
        put_chr('\n');
        return;
      }
      if (next_fp <= fp || (next_fp & 3u)) break;
      frame++;
      pc = ret_pc;
      sp = fp + 8u;
      fp = next_fp;
      print_bt_frame_u32(frame, pc, sp, fp);
    }
    return;
  }

  put_err("pdb: bt unsupported regset\n");
}
