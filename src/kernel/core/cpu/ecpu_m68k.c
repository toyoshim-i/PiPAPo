/*
 * ecpu_m68k.c — Motorola 68000 emulator core
 *
 * Implements the eCPU common interface for the m68k core.
 * Step 1: MOVE.B/W/L, MOVEQ, LEA, CLR, NOP, STOP, ILLEGAL,
 *         A-line/F-line traps, EA modes 0–4.
 * Step 2: ADD/SUB/CMP/NEG/NEGX/TST/EXT/MULU/MULS/DIVU/DIVS,
 *         ADDI/SUBI/CMPI, ADDQ/SUBQ.
 * Step 3: Bcc/BRA/BSR, DBcc, Scc, LINK, UNLK.
 * Step 4: AND/OR/EOR/NOT, shifts/rotates, bit ops, EXG.
 * Step 5: MOVEM, PEA, MOVE to/from SR/CCR, MOVE USP.
 *
 * See docs/ecpu/m68k.md for the full design.
 */

#include "kernel/core/cpu/ecpu_m68k.h"

#include <string.h> /* memset */

#include "kernel/core/cpu/cpu.h"
#include "kernel/core/exec/image_alloc.h"
#include "kernel/core/mm/mem_region.h"

/* ── Trap fire helper ───────────────────────────────────────────────────── */

static int m68k_fire_trap(m68k_state_t *cpu, int trap_type, uint32_t param) {
  if (cpu->trap_handler) {
    int rc =
        cpu->trap_handler((cpu_state_t *)cpu, trap_type, param, cpu->trap_ctx);
    if (rc == CPU_TRAP_EXIT) cpu->step_trap_exit = 1;
    return rc;
  }
  return CPU_TRAP_UNHANDLED;
}

/* ── Effective Address decoder ──────────────────────────────────────────── */

ea_result_t m68k_decode_ea(m68k_state_t *cpu, uint8_t mode, uint8_t reg,
                           uint8_t size) {
  ea_result_t ea;
  ea.value = 0;

  switch (mode) {
    case 0: /* Dn — data register direct */
      ea.type = EA_TYPE_DATA_REG;
      ea.addr = reg;
      break;

    case 1: /* An — address register direct */
      ea.type = EA_TYPE_ADDR_REG;
      ea.addr = reg;
      break;

    case 2: /* (An) — address register indirect */
      ea.type = EA_TYPE_MEMORY;
      ea.addr = cpu->a[reg];
      break;

    case 3: /* (An)+ — post-increment */
      ea.type = EA_TYPE_MEMORY;
      ea.addr = cpu->a[reg];
      cpu->a[reg] += m68k_size_bytes(size);
      /* A7 byte ops use 2-byte increment to keep SP aligned */
      if (size == M68K_SIZE_BYTE && reg == 7) cpu->a[reg]++;
      break;

    case 4: /* -(An) — pre-decrement */
      if (size == M68K_SIZE_BYTE && reg == 7) cpu->a[reg]--;
      cpu->a[reg] -= m68k_size_bytes(size);
      ea.type = EA_TYPE_MEMORY;
      ea.addr = cpu->a[reg];
      break;

    case 5: /* d16(An) — displacement indirect */
      ea.type = EA_TYPE_MEMORY;
      ea.addr = cpu->a[reg] + (int16_t)m68k_fetch16(cpu);
      break;

    case 6: { /* d8(An,Xn) — index indirect */
      uint16_t ext = m68k_fetch16(cpu);
      int8_t disp = ext & 0xFF;
      int xreg = (ext >> 12) & 7;
      int32_t xval;
      if (ext & 0x8000)
        xval = (int32_t)cpu->a[xreg];
      else
        xval = (int32_t)cpu->d[xreg];
      if (!(ext & 0x0800)) xval = (int16_t)(xval & 0xFFFF);
      ea.type = EA_TYPE_MEMORY;
      ea.addr = cpu->a[reg] + disp + xval;
      break;
    }

    case 7:
      switch (reg) {
        case 0: /* abs.W — absolute short (sign-extended) */
          ea.type = EA_TYPE_MEMORY;
          ea.addr = (uint32_t)(int16_t)m68k_fetch16(cpu);
          break;
        case 1: /* abs.L — absolute long */
          ea.type = EA_TYPE_MEMORY;
          ea.addr = m68k_fetch32(cpu);
          break;
        case 2: { /* d16(PC) — PC-relative displacement */
          uint32_t base = cpu->pc;
          ea.type = EA_TYPE_MEMORY;
          ea.addr = base + (int16_t)m68k_fetch16(cpu);
          break;
        }
        case 3: { /* d8(PC,Xn) — PC-relative index */
          uint32_t base = cpu->pc;
          uint16_t ext = m68k_fetch16(cpu);
          int8_t disp = ext & 0xFF;
          int xreg = (ext >> 12) & 7;
          int32_t xval;
          if (ext & 0x8000)
            xval = (int32_t)cpu->a[xreg];
          else
            xval = (int32_t)cpu->d[xreg];
          if (!(ext & 0x0800)) xval = (int16_t)(xval & 0xFFFF);
          ea.type = EA_TYPE_MEMORY;
          ea.addr = base + disp + xval;
          break;
        }
        case 4: /* #imm — immediate */
          ea.type = EA_TYPE_IMMEDIATE;
          if (size == M68K_SIZE_LONG)
            ea.value = m68k_fetch32(cpu);
          else
            ea.value = m68k_fetch16(cpu);
          if (size == M68K_SIZE_BYTE) ea.value &= 0xFF;
          break;
        default:
          /* Invalid EA mode — treat as NOP */
          ea.type = EA_TYPE_DATA_REG;
          ea.addr = 0;
          break;
      }
      break;
  }
  return ea;
}

/* ── EA read/write helpers ──────────────────────────────────────────────── */

uint32_t m68k_read_ea(m68k_state_t *cpu, ea_result_t *ea, uint8_t size) {
  switch (ea->type) {
    case EA_TYPE_DATA_REG:
      return cpu->d[ea->addr] & m68k_size_mask(size);
    case EA_TYPE_ADDR_REG:
      return cpu->a[ea->addr] & m68k_size_mask(size);
    case EA_TYPE_MEMORY:
      return m68k_read_sz(cpu, ea->addr, size);
    case EA_TYPE_IMMEDIATE:
      return ea->value;
    default:
      return 0;
  }
}

void m68k_write_ea(m68k_state_t *cpu, ea_result_t *ea, uint8_t size,
                   uint32_t val) {
  switch (ea->type) {
    case EA_TYPE_DATA_REG:
      m68k_write_d(cpu, ea->addr, val, size);
      break;
    case EA_TYPE_ADDR_REG:
      /* Address register writes are always 32-bit.
       * Word writes sign-extend to 32 bits. */
      if (size == M68K_SIZE_WORD)
        cpu->a[ea->addr] = (uint32_t)(int16_t)(val & 0xFFFF);
      else
        cpu->a[ea->addr] = val;
      break;
    case EA_TYPE_MEMORY:
      m68k_write_sz(cpu, ea->addr, val, size);
      break;
    default:
      break;
  }
}

/* ── CCR helpers for MOVE / logic ops ──────────────────────────────────── */

static void m68k_set_ccr_move(m68k_state_t *cpu, uint32_t val, uint8_t size) {
  uint32_t msb = m68k_size_msb(size);
  uint32_t mask = m68k_size_mask(size);

  /* Preserve X, clear N/Z/V/C, then set N/Z */
  uint16_t ccr = cpu->sr & M68K_FLAG_X;
  if ((val & mask) == 0) ccr |= M68K_FLAG_Z;
  if (val & msb) ccr |= M68K_FLAG_N;
  /* V and C cleared */
  cpu->sr = (cpu->sr & 0xFF00) | ccr;
}

/* ── Bit operation helper ───────────────────────────────────────────────── */

static void m68k_bitop(m68k_state_t *cpu, uint8_t ea_mode, uint8_t ea_reg,
                       uint8_t op, uint32_t bitnum) {
  /* Register operands: modulo 32 (long), memory: modulo 8 (byte) */
  if (ea_mode == 0) {
    /* Data register */
    bitnum &= 31;
    uint32_t mask = 1u << bitnum;
    cpu->sr =
        (cpu->sr & ~M68K_FLAG_Z) | ((cpu->d[ea_reg] & mask) ? 0 : M68K_FLAG_Z);
    switch (op) {
      case 1:
        cpu->d[ea_reg] ^= mask;
        break; /* BCHG */
      case 2:
        cpu->d[ea_reg] &= ~mask;
        break; /* BCLR */
      case 3:
        cpu->d[ea_reg] |= mask;
        break; /* BSET */
    }
  } else {
    /* Memory (byte) */
    bitnum &= 7;
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, M68K_SIZE_BYTE);
    uint8_t val = (uint8_t)m68k_read_ea(cpu, &ea, M68K_SIZE_BYTE);
    uint8_t mask = 1u << bitnum;
    cpu->sr = (cpu->sr & ~M68K_FLAG_Z) | ((val & mask) ? 0 : M68K_FLAG_Z);
    switch (op) {
      case 1:
        val ^= mask;
        break;
      case 2:
        val &= ~mask;
        break;
      case 3:
        val |= mask;
        break;
    }
    if (op != 0) m68k_write_ea(cpu, &ea, M68K_SIZE_BYTE, val);
  }
}

/* ── Group 0 (Immediate + Bit Operations) ───────────────────────────────── */

static void m68k_group0(m68k_state_t *cpu, uint16_t opcode) {
  uint8_t ea_mode = (opcode >> 3) & 7;
  uint8_t ea_reg = opcode & 7;

  /* Dynamic bit operations: 0000 rrr 1 tt ea (bit 8 set) */
  if (opcode & 0x0100) {
    int reg = (opcode >> 9) & 7;
    uint8_t op = (opcode >> 6) & 3; /* 0=BTST,1=BCHG,2=BCLR,3=BSET */
    m68k_bitop(cpu, ea_mode, ea_reg, op, cpu->d[reg]);
    return;
  }

  uint8_t bits_11_8 = (opcode >> 8) & 0xF;
  uint8_t size = (opcode >> 6) & 3;

  switch (bits_11_8 >> 1) {
    case 0x0: { /* ORI: 0000 0000 ss ea */
      if (size > 2) break;
      uint16_t imm16 = m68k_fetch16(cpu);
      /* ORI to CCR: 003C */
      if (opcode == 0x003C) {
        cpu->sr |= (imm16 & 0x1F);
        break;
      }
      /* ORI to SR: 007C */
      if (opcode == 0x007C) {
        cpu->sr |= imm16;
        break;
      }
      uint32_t imm = (size == M68K_SIZE_LONG)
                         ? ((uint32_t)imm16 << 16) | m68k_fetch16(cpu)
                         : imm16;
      if (size == M68K_SIZE_BYTE) imm &= 0xFF;
      ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, size);
      uint32_t dst = m68k_read_ea(cpu, &ea, size);
      uint32_t result = dst | imm;
      m68k_write_ea(cpu, &ea, size, result);
      m68k_set_ccr_move(cpu, result, size);
      break;
    }
    case 0x1: { /* ANDI: 0000 0010 ss ea */
      if (size > 2) break;
      uint16_t imm16 = m68k_fetch16(cpu);
      /* ANDI to CCR: 023C */
      if (opcode == 0x023C) {
        cpu->sr = (cpu->sr & 0xFF00) | ((cpu->sr & imm16) & 0xFF);
        break;
      }
      /* ANDI to SR: 027C */
      if (opcode == 0x027C) {
        cpu->sr &= imm16;
        break;
      }
      uint32_t imm = (size == M68K_SIZE_LONG)
                         ? ((uint32_t)imm16 << 16) | m68k_fetch16(cpu)
                         : imm16;
      if (size == M68K_SIZE_BYTE) imm &= 0xFF;
      ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, size);
      uint32_t dst = m68k_read_ea(cpu, &ea, size);
      uint32_t result = dst & imm;
      m68k_write_ea(cpu, &ea, size, result);
      m68k_set_ccr_move(cpu, result, size);
      break;
    }
    case 0x2: { /* SUBI: 0000 0100 ss ea */
      if (size > 2) break;
      uint32_t imm =
          (size == M68K_SIZE_LONG) ? m68k_fetch32(cpu) : m68k_fetch16(cpu);
      if (size == M68K_SIZE_BYTE) imm &= 0xFF;
      ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, size);
      uint32_t dst = m68k_read_ea(cpu, &ea, size);
      uint32_t result = m68k_alu_sub(cpu, imm, dst, size);
      m68k_write_ea(cpu, &ea, size, result);
      break;
    }
    case 0x3: { /* ADDI: 0000 0110 ss ea */
      if (size > 2) break;
      uint32_t imm =
          (size == M68K_SIZE_LONG) ? m68k_fetch32(cpu) : m68k_fetch16(cpu);
      if (size == M68K_SIZE_BYTE) imm &= 0xFF;
      ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, size);
      uint32_t dst = m68k_read_ea(cpu, &ea, size);
      uint32_t result = m68k_alu_add(cpu, imm, dst, size);
      m68k_write_ea(cpu, &ea, size, result);
      break;
    }
    case 0x4: { /* Static bit ops: 0000 1000 tt ea + #bitnum */
      uint8_t op = (opcode >> 6) & 3;
      uint16_t bitnum = m68k_fetch16(cpu);
      m68k_bitop(cpu, ea_mode, ea_reg, op, bitnum);
      break;
    }
    case 0x5: { /* EORI: 0000 1010 ss ea */
      if (size > 2) break;
      uint16_t imm16 = m68k_fetch16(cpu);
      /* EORI to CCR: 0A3C */
      if (opcode == 0x0A3C) {
        cpu->sr ^= (imm16 & 0x1F);
        break;
      }
      /* EORI to SR: 0A7C */
      if (opcode == 0x0A7C) {
        cpu->sr ^= imm16;
        break;
      }
      uint32_t imm = (size == M68K_SIZE_LONG)
                         ? ((uint32_t)imm16 << 16) | m68k_fetch16(cpu)
                         : imm16;
      if (size == M68K_SIZE_BYTE) imm &= 0xFF;
      ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, size);
      uint32_t dst = m68k_read_ea(cpu, &ea, size);
      uint32_t result = dst ^ imm;
      m68k_write_ea(cpu, &ea, size, result);
      m68k_set_ccr_move(cpu, result, size);
      break;
    }
    case 0x6: { /* CMPI: 0000 1100 ss ea */
      if (size > 2) break;
      uint32_t imm =
          (size == M68K_SIZE_LONG) ? m68k_fetch32(cpu) : m68k_fetch16(cpu);
      if (size == M68K_SIZE_BYTE) imm &= 0xFF;
      ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, size);
      uint32_t dst = m68k_read_ea(cpu, &ea, size);
      m68k_alu_cmp(cpu, imm, dst, size);
      break;
    }
    default:
      break;
  }
}

/* ── MOVE.B/W/L ─────────────────────────────────────────────────────────── */

static void m68k_move(m68k_state_t *cpu, uint16_t opcode, uint8_t size) {
  /* Source EA: bits 5–3 = mode, bits 2–0 = reg */
  uint8_t src_mode = (opcode >> 3) & 7;
  uint8_t src_reg = opcode & 7;

  /* Destination EA: bits 11–9 = reg, bits 8–6 = mode (reversed!) */
  uint8_t dst_reg = (opcode >> 9) & 7;
  uint8_t dst_mode = (opcode >> 6) & 7;

  ea_result_t src_ea = m68k_decode_ea(cpu, src_mode, src_reg, size);
  uint32_t val = m68k_read_ea(cpu, &src_ea, size);

  ea_result_t dst_ea = m68k_decode_ea(cpu, dst_mode, dst_reg, size);
  m68k_write_ea(cpu, &dst_ea, size, val);

  /* MOVEA (dst mode 1 = address register) does not set flags */
  if (dst_mode != 1) m68k_set_ccr_move(cpu, val, size);
}

/* ── MOVEQ ──────────────────────────────────────────────────────────────── */

static void m68k_moveq(m68k_state_t *cpu, uint16_t opcode) {
  int reg = (opcode >> 9) & 7;
  int32_t val = (int8_t)(opcode & 0xFF); /* sign-extend 8→32 */
  cpu->d[reg] = (uint32_t)val;
  m68k_set_ccr_move(cpu, (uint32_t)val, M68K_SIZE_LONG);
}

/* ── Condition code evaluator ───────────────────────────────────────────── */

static int m68k_test_cc(m68k_state_t *cpu, uint8_t cc) {
  uint16_t sr = cpu->sr;
  int n = (sr >> 3) & 1;
  int z = (sr >> 2) & 1;
  int v = (sr >> 1) & 1;
  int c = sr & 1;

  switch (cc) {
    case 0x0:
      return 1; /* T  — always true */
    case 0x1:
      return 0; /* F  — always false */
    case 0x2:
      return !c && !z; /* HI — higher (unsigned) */
    case 0x3:
      return c || z; /* LS — lower or same */
    case 0x4:
      return !c; /* CC — carry clear */
    case 0x5:
      return c; /* CS — carry set */
    case 0x6:
      return !z; /* NE — not equal */
    case 0x7:
      return z; /* EQ — equal */
    case 0x8:
      return !v; /* VC — overflow clear */
    case 0x9:
      return v; /* VS — overflow set */
    case 0xA:
      return !n; /* PL — plus */
    case 0xB:
      return n; /* MI — minus */
    case 0xC:
      return n == v; /* GE — greater or equal (signed) */
    case 0xD:
      return n != v; /* LT — less than (signed) */
    case 0xE:
      return !z && (n == v); /* GT — greater than (signed) */
    case 0xF:
      return z || (n != v); /* LE — less or equal (signed) */
    default:
      return 0;
  }
}

/* ── Group 4 (Miscellaneous) ────────────────────────────────────────────── */

static int m68k_group4(m68k_state_t *cpu, uint16_t opcode) {
  uint8_t bits_11_8 = (opcode >> 8) & 0xF;
  uint8_t bits_7_6 = (opcode >> 6) & 3;
  uint8_t ea_mode = (opcode >> 3) & 7;
  uint8_t ea_reg = opcode & 7;

  /* NOP: 4E71 */
  if (opcode == 0x4E71) return 0;

  /* STOP: 4E72 */
  if (opcode == 0x4E72) {
    uint16_t imm = m68k_fetch16(cpu);
    cpu->sr = imm;
    cpu->stopped = 1;
    int rc = m68k_fire_trap(cpu, CPU_TRAP_HALT, imm);
    if (rc == CPU_TRAP_EXIT) return -1;
    return 0;
  }

  /* ILLEGAL: 4AFC */
  if (opcode == 0x4AFC) {
    int rc = m68k_fire_trap(cpu, CPU_TRAP_ILLEGAL, opcode);
    if (rc == CPU_TRAP_EXIT) return -1;
    return 0;
  }

  /* RTS: 4E75 */
  if (opcode == 0x4E75) {
    cpu->pc = m68k_pop32(cpu);
    return 0;
  }

  /* RTE: 4E73 */
  if (opcode == 0x4E73) {
    cpu->sr = m68k_pop16(cpu);
    cpu->pc = m68k_pop32(cpu);
    return 0;
  }

  /* TRAP #n: 4E40–4E4F */
  if ((opcode & 0xFFF0) == 0x4E40) {
    int n = opcode & 0x0F;
    int rc = m68k_fire_trap(cpu, CPU_TRAP_SWI, n);
    if (rc == CPU_TRAP_EXIT) return -1;
    if (rc == CPU_TRAP_HANDLED) return 0;
    /* Unhandled: push SR+PC, load vector */
    m68k_push32(cpu, cpu->pc);
    m68k_push16(cpu, cpu->sr);
    cpu->sr |= M68K_SR_S;
    cpu->pc = m68k_read32(cpu, (32 + n) * 4);
    return 0;
  }

  /* CHK.W: 0100 rrr 110 ea  (bits 8–6 = 110) */
  if (bits_7_6 == 2 && (bits_11_8 & 1)) {
    int reg = (opcode >> 9) & 7;
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, M68K_SIZE_WORD);
    int16_t src = (int16_t)m68k_read_ea(cpu, &ea, M68K_SIZE_WORD);
    int16_t dn = (int16_t)(cpu->d[reg] & 0xFFFF);
    if (dn < 0) {
      cpu->sr |= M68K_FLAG_N;
      int rc = m68k_fire_trap(cpu, CPU_TRAP_ILLEGAL, 6);
      if (rc == CPU_TRAP_EXIT) return -1;
    } else if (dn > src) {
      cpu->sr &= ~M68K_FLAG_N;
      int rc = m68k_fire_trap(cpu, CPU_TRAP_ILLEGAL, 6);
      if (rc == CPU_TRAP_EXIT) return -1;
    }
    return 0;
  }

  /* LEA: 0100 rrr 111 ea  (bits 8–6 = 111) */
  if (bits_7_6 == 3 && (bits_11_8 & 1)) {
    int reg = (opcode >> 9) & 7;
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, M68K_SIZE_LONG);
    if (ea.type == EA_TYPE_MEMORY) cpu->a[reg] = ea.addr;
    return 0;
  }

  /* NEG: 0100 0100 ss ea  (bits 11–8 = 0100) */
  if (bits_11_8 == 0x4 && bits_7_6 <= 2) {
    uint8_t size = bits_7_6;
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, size);
    uint32_t dst = m68k_read_ea(cpu, &ea, size);
    uint32_t result = m68k_alu_neg(cpu, dst, size);
    m68k_write_ea(cpu, &ea, size, result);
    return 0;
  }

  /* NEGX: 0100 0000 ss ea  (bits 11–8 = 0000) */
  if (bits_11_8 == 0x0 && bits_7_6 <= 2) {
    uint8_t size = bits_7_6;
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, size);
    uint32_t dst = m68k_read_ea(cpu, &ea, size);
    uint32_t result = m68k_alu_negx(cpu, dst, size);
    m68k_write_ea(cpu, &ea, size, result);
    return 0;
  }

  /* TAS: 0100 1010 11 ea  (bits 11–8 = 1010, bits_7_6 = 3) */
  if (bits_11_8 == 0xA && bits_7_6 == 3) {
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, M68K_SIZE_BYTE);
    uint32_t val = m68k_read_ea(cpu, &ea, M68K_SIZE_BYTE);
    m68k_alu_tst(cpu, val, M68K_SIZE_BYTE);
    m68k_write_ea(cpu, &ea, M68K_SIZE_BYTE, val | 0x80);
    return 0;
  }

  /* TST: 0100 1010 ss ea  (bits 11–8 = 1010) */
  if (bits_11_8 == 0xA && bits_7_6 <= 2) {
    uint8_t size = bits_7_6;
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, size);
    uint32_t val = m68k_read_ea(cpu, &ea, size);
    m68k_alu_tst(cpu, val, size);
    return 0;
  }

  /* NBCD: 0100 1000 00 ea  (bits 11–8 = 1000, bits_7_6 = 0) */
  if (bits_11_8 == 0x8 && bits_7_6 == 0) {
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, M68K_SIZE_BYTE);
    uint32_t val = m68k_read_ea(cpu, &ea, M68K_SIZE_BYTE);
    uint32_t result = m68k_alu_nbcd(cpu, (uint8_t)val);
    m68k_write_ea(cpu, &ea, M68K_SIZE_BYTE, result);
    return 0;
  }

  /* EXT.W: 0100 1000 10 000 Dn  (4880–4887) */
  if ((opcode & 0xFFF8) == 0x4880) {
    int reg = opcode & 7;
    uint16_t val = (uint16_t)(int8_t)(cpu->d[reg] & 0xFF);
    cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | val;
    m68k_alu_tst(cpu, val, M68K_SIZE_WORD);
    return 0;
  }

  /* EXT.L: 0100 1000 11 000 Dn  (48C0–48C7) */
  if ((opcode & 0xFFF8) == 0x48C0) {
    int reg = opcode & 7;
    uint32_t val = (uint32_t)(int16_t)(cpu->d[reg] & 0xFFFF);
    cpu->d[reg] = val;
    m68k_alu_tst(cpu, val, M68K_SIZE_LONG);
    return 0;
  }

  /* MOVEM register-to-memory: 0100 1000 1s ea  (ea_mode != 0) */
  if (bits_11_8 == 0x8 && (opcode & 0x0080) && ea_mode != 0) {
    uint8_t sz = (opcode & 0x0040) ? M68K_SIZE_LONG : M68K_SIZE_WORD;
    uint16_t mask = m68k_fetch16(cpu);
    uint32_t nbytes = m68k_size_bytes(sz);

    if (ea_mode == 4) {
      /* Pre-decrement -(An): reversed mask order A7→D0 */
      uint32_t addr = cpu->a[ea_reg];
      for (int i = 0; i < 16; i++) {
        if (mask & (1 << i)) {
          addr -= nbytes;
          uint32_t val;
          if (i < 8)
            val = cpu->a[7 - i];
          else
            val = cpu->d[15 - i];
          m68k_write_sz(cpu, addr, val, sz);
        }
      }
      cpu->a[ea_reg] = addr;
    } else {
      /* Other modes: normal order D0→A7 */
      ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, sz);
      uint32_t addr = ea.addr;
      for (int i = 0; i < 16; i++) {
        if (mask & (1 << i)) {
          uint32_t val = (i < 8) ? cpu->d[i] : cpu->a[i - 8];
          m68k_write_sz(cpu, addr, val, sz);
          addr += nbytes;
        }
      }
    }
    return 0;
  }

  /* MOVEM memory-to-register: 0100 1100 1s ea */
  if (bits_11_8 == 0xC && (opcode & 0x0080)) {
    uint8_t sz = (opcode & 0x0040) ? M68K_SIZE_LONG : M68K_SIZE_WORD;
    uint16_t mask = m68k_fetch16(cpu);
    uint32_t nbytes = m68k_size_bytes(sz);

    if (ea_mode == 3) {
      /* Post-increment (An)+: normal order, update An */
      uint32_t addr = cpu->a[ea_reg];
      for (int i = 0; i < 16; i++) {
        if (mask & (1 << i)) {
          uint32_t val = m68k_read_sz(cpu, addr, sz);
          if (sz == M68K_SIZE_WORD) val = (uint32_t)(int16_t)(uint16_t)val;
          if (i < 8)
            cpu->d[i] = val;
          else
            cpu->a[i - 8] = val;
          addr += nbytes;
        }
      }
      cpu->a[ea_reg] = addr;
    } else {
      /* Other modes: use decoded EA address */
      ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, sz);
      uint32_t addr = ea.addr;
      for (int i = 0; i < 16; i++) {
        if (mask & (1 << i)) {
          uint32_t val = m68k_read_sz(cpu, addr, sz);
          if (sz == M68K_SIZE_WORD) val = (uint32_t)(int16_t)(uint16_t)val;
          if (i < 8)
            cpu->d[i] = val;
          else
            cpu->a[i - 8] = val;
          addr += nbytes;
        }
      }
    }
    return 0;
  }

  /* SWAP: 0100 1000 01 000 Dn  (4840–4847) */
  if ((opcode & 0xFFF8) == 0x4840) {
    int reg = opcode & 7;
    cpu->d[reg] = (cpu->d[reg] >> 16) | (cpu->d[reg] << 16);
    m68k_alu_tst(cpu, cpu->d[reg], M68K_SIZE_LONG);
    return 0;
  }

  /* PEA: 0100 1000 01 ea  (4840–487F, ea_mode != 0) */
  if ((opcode & 0xFFC0) == 0x4840 && ea_mode != 0) {
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, M68K_SIZE_LONG);
    if (ea.type == EA_TYPE_MEMORY) m68k_push32(cpu, ea.addr);
    return 0;
  }

  /* NOT: 0100 0110 ss ea  (bits 11–8 = 0110) */
  if (bits_11_8 == 0x6 && bits_7_6 <= 2) {
    uint8_t size = bits_7_6;
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, size);
    uint32_t dst = m68k_read_ea(cpu, &ea, size);
    uint32_t result = ~dst & m68k_size_mask(size);
    m68k_write_ea(cpu, &ea, size, result);
    m68k_set_ccr_move(cpu, result, size);
    return 0;
  }

  /* MOVE from SR: 0100 0000 11 ea  (40C0–40FF) */
  if (bits_11_8 == 0x0 && bits_7_6 == 3) {
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, M68K_SIZE_WORD);
    m68k_write_ea(cpu, &ea, M68K_SIZE_WORD, cpu->sr);
    return 0;
  }

  /* MOVE to CCR: 0100 0100 11 ea  (44C0–44FF) */
  if (bits_11_8 == 0x4 && bits_7_6 == 3) {
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, M68K_SIZE_WORD);
    uint32_t val = m68k_read_ea(cpu, &ea, M68K_SIZE_WORD);
    cpu->sr = (cpu->sr & 0xFF00) | (val & 0xFF);
    return 0;
  }

  /* MOVE to SR: 0100 0110 11 ea  (46C0–46FF) */
  if (bits_11_8 == 0x6 && bits_7_6 == 3) {
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, M68K_SIZE_WORD);
    uint32_t val = m68k_read_ea(cpu, &ea, M68K_SIZE_WORD);
    cpu->sr = (uint16_t)val;
    return 0;
  }

  /* CLR: 0100 0010 ss ea  (bits 11–8 = 0010) */
  if (bits_11_8 == 0x2) {
    uint8_t size = bits_7_6; /* 00=byte, 01=word, 10=long */
    if (size > 2) return 0;  /* size 11 is invalid for CLR */
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, size);
    m68k_write_ea(cpu, &ea, size, 0);
    /* CCR: X unchanged, N=0, Z=1, V=0, C=0 */
    cpu->sr = (cpu->sr & (0xFF00 | M68K_FLAG_X)) | M68K_FLAG_Z;
    return 0;
  }

  /* JSR: 4E80–4EBF (0100 1110 10 ea) */
  if ((opcode & 0xFFC0) == 0x4E80) {
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, M68K_SIZE_LONG);
    if (ea.type == EA_TYPE_MEMORY) {
      m68k_push32(cpu, cpu->pc);
      cpu->pc = ea.addr;
    }
    return 0;
  }

  /* JMP: 4EC0–4EFF (0100 1110 11 ea) */
  if ((opcode & 0xFFC0) == 0x4EC0) {
    ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, M68K_SIZE_LONG);
    if (ea.type == EA_TYPE_MEMORY) cpu->pc = ea.addr;
    return 0;
  }

  /* LINK An, #disp: 4E50–4E57 */
  if ((opcode & 0xFFF8) == 0x4E50) {
    int reg = opcode & 7;
    int16_t disp = (int16_t)m68k_fetch16(cpu);
    m68k_push32(cpu, cpu->a[reg]);
    cpu->a[reg] = cpu->a[7];
    cpu->a[7] += disp;
    return 0;
  }

  /* UNLK An: 4E58–4E5F */
  if ((opcode & 0xFFF8) == 0x4E58) {
    int reg = opcode & 7;
    cpu->a[7] = cpu->a[reg];
    cpu->a[reg] = m68k_pop32(cpu);
    return 0;
  }

  /* MOVE USP: 4E60–4E6F */
  if ((opcode & 0xFFF0) == 0x4E60) {
    int reg = opcode & 7;
    if (opcode & 0x0008) {
      /* USP → An */
      cpu->a[reg] = cpu->usp;
    } else {
      /* An → USP */
      cpu->usp = cpu->a[reg];
    }
    return 0;
  }

  /* Unimplemented group 4 — treat as NOP for now */
  return 0;
}

/* ── Main decode loop ───────────────────────────────────────────────────── */

static int ecpu_m68k_run(cpu_state_t *state) {
  m68k_state_t *cpu = (m68k_state_t *)state;
  cpu->stopped = 0;
  cpu->step_trap_exit = 0;

  for (;;) {
    if (cpu->stopped) return 0;

    uint16_t opcode = m68k_fetch16(cpu);

    switch (opcode >> 12) {
      case 0x0: /* Group 0: immediate/bit ops */
        m68k_group0(cpu, opcode);
        break;

      case 0x1: /* MOVE.B */
        m68k_move(cpu, opcode, M68K_SIZE_BYTE);
        break;

      case 0x2: /* MOVE.L */
        m68k_move(cpu, opcode, M68K_SIZE_LONG);
        break;

      case 0x3: /* MOVE.W */
        m68k_move(cpu, opcode, M68K_SIZE_WORD);
        break;

      case 0x4: /* Group 4: miscellaneous */
        if (m68k_group4(cpu, opcode) < 0) return 0;
        break;

      case 0x5: { /* Group 5: ADDQ/SUBQ/Scc/DBcc */
        uint8_t size5 = (opcode >> 6) & 3;
        if (size5 == 3) {
          /* Scc/DBcc: 0101 cccc 11 ea */
          uint8_t cc5 = (opcode >> 8) & 0xF;
          uint8_t ea_m5s = (opcode >> 3) & 7;
          uint8_t ea_r5s = opcode & 7;
          if (ea_m5s == 1) {
            /* DBcc Dn, disp: mode=001 reg=Dn */
            int16_t disp = (int16_t)m68k_fetch16(cpu);
            if (!m68k_test_cc(cpu, cc5)) {
              int16_t val = (int16_t)(cpu->d[ea_r5s] & 0xFFFF) - 1;
              cpu->d[ea_r5s] = (cpu->d[ea_r5s] & 0xFFFF0000) | ((uint16_t)val);
              if (val != -1) cpu->pc = cpu->pc - 2 + disp;
            }
          } else {
            /* Scc <ea> */
            ea_result_t ea5s =
                m68k_decode_ea(cpu, ea_m5s, ea_r5s, M68K_SIZE_BYTE);
            uint8_t val = m68k_test_cc(cpu, cc5) ? 0xFF : 0x00;
            m68k_write_ea(cpu, &ea5s, M68K_SIZE_BYTE, val);
          }
          break;
        }
        uint8_t data5 = (opcode >> 9) & 7;
        if (data5 == 0) data5 = 8; /* ADDQ/SUBQ: 0 means 8 */
        uint8_t ea_m5 = (opcode >> 3) & 7;
        uint8_t ea_r5 = opcode & 7;
        ea_result_t ea5 = m68k_decode_ea(cpu, ea_m5, ea_r5, size5);
        if (opcode & 0x0100) {
          /* SUBQ */
          if (ea5.type == EA_TYPE_ADDR_REG) {
            /* SUBQ to An: full 32-bit, no flags */
            cpu->a[ea5.addr] -= data5;
          } else {
            uint32_t dst5 = m68k_read_ea(cpu, &ea5, size5);
            uint32_t r5 = m68k_alu_sub(cpu, data5, dst5, size5);
            m68k_write_ea(cpu, &ea5, size5, r5);
          }
        } else {
          /* ADDQ */
          if (ea5.type == EA_TYPE_ADDR_REG) {
            cpu->a[ea5.addr] += data5;
          } else {
            uint32_t dst5 = m68k_read_ea(cpu, &ea5, size5);
            uint32_t r5 = m68k_alu_add(cpu, data5, dst5, size5);
            m68k_write_ea(cpu, &ea5, size5, r5);
          }
        }
        break;
      }

      case 0x6: { /* Group 6: Bcc/BSR/BRA */
        uint8_t cc6 = (opcode >> 8) & 0xF;
        int8_t disp8 = (int8_t)(opcode & 0xFF);
        int32_t disp;
        uint32_t base = cpu->pc; /* PC after opcode fetch */
        if (disp8 == 0)
          disp = (int16_t)m68k_fetch16(cpu);
        else
          disp = disp8;
        if (cc6 == 0) {
          /* BRA */
          cpu->pc = base + disp;
        } else if (cc6 == 1) {
          /* BSR */
          m68k_push32(cpu, cpu->pc);
          cpu->pc = base + disp;
        } else {
          /* Bcc */
          if (m68k_test_cc(cpu, cc6)) cpu->pc = base + disp;
        }
        break;
      }

      case 0x7: /* MOVEQ */
        m68k_moveq(cpu, opcode);
        break;

      case 0x8: { /* Group 8: OR/DIV/SBCD */
        uint8_t omode8 = (opcode >> 6) & 7;
        uint8_t ea_m8 = (opcode >> 3) & 7;
        uint8_t ea_r8 = opcode & 7;
        int reg8 = (opcode >> 9) & 7;
        if (omode8 == 3) {
          /* DIVU: 1000 rrr 011 ea */
          ea_result_t ea8 = m68k_decode_ea(cpu, ea_m8, ea_r8, M68K_SIZE_WORD);
          uint16_t src8 = (uint16_t)m68k_read_ea(cpu, &ea8, M68K_SIZE_WORD);
          uint32_t result8;
          int rc8 = m68k_alu_divu(cpu, cpu->d[reg8], src8, &result8);
          if (rc8 < 0) {
            /* Divide by zero — fire trap */
            int rc = m68k_fire_trap(cpu, CPU_TRAP_ILLEGAL, 5);
            if (rc == CPU_TRAP_EXIT) return 0;
          } else if (rc8 == 0) {
            cpu->d[reg8] = result8;
          }
          /* rc8 == 1: overflow, register unchanged */
        } else if (omode8 == 7) {
          /* DIVS: 1000 rrr 111 ea */
          ea_result_t ea8 = m68k_decode_ea(cpu, ea_m8, ea_r8, M68K_SIZE_WORD);
          int16_t src8 = (int16_t)m68k_read_ea(cpu, &ea8, M68K_SIZE_WORD);
          uint32_t result8;
          int rc8 = m68k_alu_divs(cpu, (int32_t)cpu->d[reg8], src8, &result8);
          if (rc8 < 0) {
            int rc = m68k_fire_trap(cpu, CPU_TRAP_ILLEGAL, 5);
            if (rc == CPU_TRAP_EXIT) return 0;
          } else if (rc8 == 0) {
            cpu->d[reg8] = result8;
          }
        }
        /* OR */
        if (omode8 < 3) {
          /* OR <ea>, Dn */
          uint8_t size8 = omode8;
          ea_result_t ea8 = m68k_decode_ea(cpu, ea_m8, ea_r8, size8);
          uint32_t src8 = m68k_read_ea(cpu, &ea8, size8);
          uint32_t r8 = (cpu->d[reg8] | src8) & m68k_size_mask(size8);
          m68k_write_d(cpu, reg8, r8, size8);
          m68k_set_ccr_move(cpu, r8, size8);
        } else if (omode8 == 4 && ea_m8 <= 1) {
          /* SBCD: 1000 rrr 100 00m rrr */
          if (ea_m8 == 0) {
            /* SBCD Dy, Dx */
            uint8_t r8 = (uint8_t)m68k_alu_sbcd(cpu, (uint8_t)cpu->d[ea_r8],
                                                (uint8_t)cpu->d[reg8]);
            m68k_write_d(cpu, reg8, r8, M68K_SIZE_BYTE);
          } else {
            /* SBCD -(Ay), -(Ax) */
            cpu->a[ea_r8] -= 1;
            cpu->a[reg8] -= 1;
            uint8_t src8b = m68k_read8(cpu, cpu->a[ea_r8]);
            uint8_t dst8b = m68k_read8(cpu, cpu->a[reg8]);
            uint8_t r8 = (uint8_t)m68k_alu_sbcd(cpu, src8b, dst8b);
            m68k_write8(cpu, cpu->a[reg8], r8);
          }
        } else if (omode8 >= 4 && omode8 <= 6) {
          /* OR Dn, <ea> */
          uint8_t size8 = omode8 & 3;
          ea_result_t ea8 = m68k_decode_ea(cpu, ea_m8, ea_r8, size8);
          uint32_t dst8 = m68k_read_ea(cpu, &ea8, size8);
          uint32_t r8 = (cpu->d[reg8] | dst8) & m68k_size_mask(size8);
          m68k_write_ea(cpu, &ea8, size8, r8);
          m68k_set_ccr_move(cpu, r8, size8);
        }
        break;
      }

      case 0x9: { /* Group 9: SUB/SUBA */
        int reg9 = (opcode >> 9) & 7;
        uint8_t omode9 = (opcode >> 6) & 7;
        uint8_t ea_m9 = (opcode >> 3) & 7;
        uint8_t ea_r9 = opcode & 7;

        if (omode9 == 3) {
          /* SUBA.W: src sign-extended to 32, sub from An */
          ea_result_t ea9 = m68k_decode_ea(cpu, ea_m9, ea_r9, M68K_SIZE_WORD);
          int16_t src9 = (int16_t)m68k_read_ea(cpu, &ea9, M68K_SIZE_WORD);
          cpu->a[reg9] -= (int32_t)src9;
        } else if (omode9 == 7) {
          /* SUBA.L */
          ea_result_t ea9 = m68k_decode_ea(cpu, ea_m9, ea_r9, M68K_SIZE_LONG);
          uint32_t src9 = m68k_read_ea(cpu, &ea9, M68K_SIZE_LONG);
          cpu->a[reg9] -= src9;
        } else {
          /* SUB: omode 0-2 = <ea>,Dn; 4-6 = Dn,<ea> */
          uint8_t size9 = omode9 & 3;
          if (size9 > 2) break;
          if (omode9 >= 4 && ea_m9 <= 1) {
            /* SUBX: 1001 rrr 1 ss 00m rrr */
            if (ea_m9 == 0) {
              /* SUBX Dy, Dx */
              uint32_t r9 =
                  m68k_alu_subx(cpu, cpu->d[ea_r9], cpu->d[reg9], size9);
              m68k_write_d(cpu, reg9, r9, size9);
            } else {
              /* SUBX -(Ay), -(Ax) */
              uint32_t nb = m68k_size_bytes(size9);
              cpu->a[ea_r9] -= nb;
              cpu->a[reg9] -= nb;
              uint32_t src9 = m68k_read_sz(cpu, cpu->a[ea_r9], size9);
              uint32_t dst9 = m68k_read_sz(cpu, cpu->a[reg9], size9);
              uint32_t r9 = m68k_alu_subx(cpu, src9, dst9, size9);
              m68k_write_sz(cpu, cpu->a[reg9], r9, size9);
            }
          } else if (omode9 < 3) {
            /* SUB <ea>, Dn */
            ea_result_t ea9 = m68k_decode_ea(cpu, ea_m9, ea_r9, size9);
            uint32_t src9 = m68k_read_ea(cpu, &ea9, size9);
            uint32_t r9 = m68k_alu_sub(cpu, src9, cpu->d[reg9], size9);
            m68k_write_d(cpu, reg9, r9, size9);
          } else {
            /* SUB Dn, <ea> */
            ea_result_t ea9 = m68k_decode_ea(cpu, ea_m9, ea_r9, size9);
            uint32_t dst9 = m68k_read_ea(cpu, &ea9, size9);
            uint32_t r9 = m68k_alu_sub(
                cpu, cpu->d[reg9] & m68k_size_mask(size9), dst9, size9);
            m68k_write_ea(cpu, &ea9, size9, r9);
          }
        }
        break;
      }

      case 0xA: { /* A-line trap */
        int rc = m68k_fire_trap(cpu, CPU_TRAP_ILLEGAL, opcode);
        if (rc == CPU_TRAP_EXIT) return 0;
        break;
      }

      case 0xB: { /* Group B: CMP/CMPA/CMPM/EOR */
        int regB = (opcode >> 9) & 7;
        uint8_t omodeB = (opcode >> 6) & 7;
        uint8_t ea_mB = (opcode >> 3) & 7;
        uint8_t ea_rB = opcode & 7;

        if (omodeB == 3) {
          /* CMPA.W */
          ea_result_t eaB = m68k_decode_ea(cpu, ea_mB, ea_rB, M68K_SIZE_WORD);
          int32_t srcB = (int16_t)m68k_read_ea(cpu, &eaB, M68K_SIZE_WORD);
          m68k_alu_cmp(cpu, (uint32_t)srcB, cpu->a[regB], M68K_SIZE_LONG);
        } else if (omodeB == 7) {
          /* CMPA.L */
          ea_result_t eaB = m68k_decode_ea(cpu, ea_mB, ea_rB, M68K_SIZE_LONG);
          uint32_t srcB = m68k_read_ea(cpu, &eaB, M68K_SIZE_LONG);
          m68k_alu_cmp(cpu, srcB, cpu->a[regB], M68K_SIZE_LONG);
        } else if (omodeB < 3) {
          /* CMP <ea>, Dn */
          uint8_t sizeB = omodeB;
          ea_result_t eaB = m68k_decode_ea(cpu, ea_mB, ea_rB, sizeB);
          uint32_t srcB = m68k_read_ea(cpu, &eaB, sizeB);
          m68k_alu_cmp(cpu, srcB, cpu->d[regB], sizeB);
        } else if (ea_mB == 1) {
          /* CMPM (An)+, (An)+ */
          uint8_t sizeB = omodeB & 3;
          if (sizeB > 2) break;
          ea_result_t eaS = m68k_decode_ea(cpu, 3, ea_rB, sizeB);
          uint32_t srcB = m68k_read_ea(cpu, &eaS, sizeB);
          ea_result_t eaD = m68k_decode_ea(cpu, 3, regB, sizeB);
          uint32_t dstB = m68k_read_ea(cpu, &eaD, sizeB);
          m68k_alu_cmp(cpu, srcB, dstB, sizeB);
        } else if (omodeB >= 4 && omodeB <= 6) {
          /* EOR Dn, <ea> */
          uint8_t sizeB = omodeB & 3;
          ea_result_t eaB = m68k_decode_ea(cpu, ea_mB, ea_rB, sizeB);
          uint32_t dstB = m68k_read_ea(cpu, &eaB, sizeB);
          uint32_t rB = (cpu->d[regB] ^ dstB) & m68k_size_mask(sizeB);
          m68k_write_ea(cpu, &eaB, sizeB, rB);
          m68k_set_ccr_move(cpu, rB, sizeB);
        }
        break;
      }

      case 0xC: { /* Group C: AND/MUL/ABCD/EXG */
        uint8_t omodeC = (opcode >> 6) & 7;
        uint8_t ea_mC = (opcode >> 3) & 7;
        uint8_t ea_rC = opcode & 7;
        int regC = (opcode >> 9) & 7;
        if (omodeC == 3) {
          /* MULU: 1100 rrr 011 ea */
          ea_result_t eaC = m68k_decode_ea(cpu, ea_mC, ea_rC, M68K_SIZE_WORD);
          uint16_t srcC = (uint16_t)m68k_read_ea(cpu, &eaC, M68K_SIZE_WORD);
          cpu->d[regC] = m68k_alu_mulu(cpu, srcC, (uint16_t)cpu->d[regC]);
        } else if (omodeC == 7) {
          /* MULS: 1100 rrr 111 ea */
          ea_result_t eaC = m68k_decode_ea(cpu, ea_mC, ea_rC, M68K_SIZE_WORD);
          int16_t srcC = (int16_t)m68k_read_ea(cpu, &eaC, M68K_SIZE_WORD);
          cpu->d[regC] = m68k_alu_muls(cpu, srcC, (int16_t)cpu->d[regC]);
        }
        /* EXG: C140/C148/C188 patterns */
        if ((opcode & 0xF1F8) == 0xC140) {
          /* EXG Dx, Dy */
          int ry = opcode & 7;
          uint32_t tmp = cpu->d[regC];
          cpu->d[regC] = cpu->d[ry];
          cpu->d[ry] = tmp;
        } else if ((opcode & 0xF1F8) == 0xC148) {
          /* EXG Ax, Ay */
          int ry = opcode & 7;
          uint32_t tmp = cpu->a[regC];
          cpu->a[regC] = cpu->a[ry];
          cpu->a[ry] = tmp;
        } else if ((opcode & 0xF1F8) == 0xC188) {
          /* EXG Dx, Ay */
          int ry = opcode & 7;
          uint32_t tmp = cpu->d[regC];
          cpu->d[regC] = cpu->a[ry];
          cpu->a[ry] = tmp;
        } else if (omodeC < 3) {
          /* AND <ea>, Dn */
          uint8_t sizeC = omodeC;
          ea_result_t eaC = m68k_decode_ea(cpu, ea_mC, ea_rC, sizeC);
          uint32_t srcC = m68k_read_ea(cpu, &eaC, sizeC);
          uint32_t rC = (cpu->d[regC] & srcC) & m68k_size_mask(sizeC);
          m68k_write_d(cpu, regC, rC, sizeC);
          m68k_set_ccr_move(cpu, rC, sizeC);
        } else if (omodeC == 4 && ea_mC <= 1) {
          /* ABCD: 1100 rrr 100 00m rrr */
          if (ea_mC == 0) {
            /* ABCD Dy, Dx */
            uint8_t rC = (uint8_t)m68k_alu_abcd(cpu, (uint8_t)cpu->d[ea_rC],
                                                (uint8_t)cpu->d[regC]);
            m68k_write_d(cpu, regC, rC, M68K_SIZE_BYTE);
          } else {
            /* ABCD -(Ay), -(Ax) */
            cpu->a[ea_rC] -= 1;
            cpu->a[regC] -= 1;
            uint8_t srcCb = m68k_read8(cpu, cpu->a[ea_rC]);
            uint8_t dstCb = m68k_read8(cpu, cpu->a[regC]);
            uint8_t rC = (uint8_t)m68k_alu_abcd(cpu, srcCb, dstCb);
            m68k_write8(cpu, cpu->a[regC], rC);
          }
        } else if (omodeC >= 4 && omodeC <= 6) {
          /* AND Dn, <ea> */
          uint8_t sizeC = omodeC & 3;
          ea_result_t eaC = m68k_decode_ea(cpu, ea_mC, ea_rC, sizeC);
          uint32_t dstC = m68k_read_ea(cpu, &eaC, sizeC);
          uint32_t rC = (cpu->d[regC] & dstC) & m68k_size_mask(sizeC);
          m68k_write_ea(cpu, &eaC, sizeC, rC);
          m68k_set_ccr_move(cpu, rC, sizeC);
        }
        break;
      }

      case 0xD: { /* Group D: ADD/ADDA */
        int regD = (opcode >> 9) & 7;
        uint8_t omodeD = (opcode >> 6) & 7;
        uint8_t ea_mD = (opcode >> 3) & 7;
        uint8_t ea_rD = opcode & 7;

        if (omodeD == 3) {
          /* ADDA.W */
          ea_result_t eaD = m68k_decode_ea(cpu, ea_mD, ea_rD, M68K_SIZE_WORD);
          int16_t srcD = (int16_t)m68k_read_ea(cpu, &eaD, M68K_SIZE_WORD);
          cpu->a[regD] += (int32_t)srcD;
        } else if (omodeD == 7) {
          /* ADDA.L */
          ea_result_t eaD = m68k_decode_ea(cpu, ea_mD, ea_rD, M68K_SIZE_LONG);
          uint32_t srcD = m68k_read_ea(cpu, &eaD, M68K_SIZE_LONG);
          cpu->a[regD] += srcD;
        } else {
          uint8_t sizeD = omodeD & 3;
          if (sizeD > 2) break;
          if (omodeD >= 4 && ea_mD <= 1) {
            /* ADDX: 1101 rrr 1 ss 00m rrr */
            if (ea_mD == 0) {
              /* ADDX Dy, Dx */
              uint32_t rD =
                  m68k_alu_addx(cpu, cpu->d[ea_rD], cpu->d[regD], sizeD);
              m68k_write_d(cpu, regD, rD, sizeD);
            } else {
              /* ADDX -(Ay), -(Ax) */
              uint32_t nb = m68k_size_bytes(sizeD);
              cpu->a[ea_rD] -= nb;
              cpu->a[regD] -= nb;
              uint32_t srcD = m68k_read_sz(cpu, cpu->a[ea_rD], sizeD);
              uint32_t dstD = m68k_read_sz(cpu, cpu->a[regD], sizeD);
              uint32_t rD = m68k_alu_addx(cpu, srcD, dstD, sizeD);
              m68k_write_sz(cpu, cpu->a[regD], rD, sizeD);
            }
          } else if (omodeD < 3) {
            /* ADD <ea>, Dn */
            ea_result_t eaD = m68k_decode_ea(cpu, ea_mD, ea_rD, sizeD);
            uint32_t srcD = m68k_read_ea(cpu, &eaD, sizeD);
            uint32_t rD = m68k_alu_add(cpu, srcD, cpu->d[regD], sizeD);
            m68k_write_d(cpu, regD, rD, sizeD);
          } else {
            /* ADD Dn, <ea> */
            ea_result_t eaD = m68k_decode_ea(cpu, ea_mD, ea_rD, sizeD);
            uint32_t dstD = m68k_read_ea(cpu, &eaD, sizeD);
            uint32_t rD = m68k_alu_add(
                cpu, cpu->d[regD] & m68k_size_mask(sizeD), dstD, sizeD);
            m68k_write_ea(cpu, &eaD, sizeD, rD);
          }
        }
        break;
      }

      case 0xE: { /* Group E: shifts/rotates */
        uint8_t sizeE = (opcode >> 6) & 3;

        if (sizeE == 3) {
          /* Memory shift: 1110 0tt d 11 ea — word, count=1 */
          uint8_t ea_mE = (opcode >> 3) & 7;
          uint8_t ea_rE = opcode & 7;
          uint8_t typeE = (opcode >> 9) & 3;
          int leftE = (opcode >> 8) & 1;
          ea_result_t eaE = m68k_decode_ea(cpu, ea_mE, ea_rE, M68K_SIZE_WORD);
          uint16_t val = (uint16_t)m68k_read_ea(cpu, &eaE, M68K_SIZE_WORD);
          uint16_t ccr = cpu->sr & M68K_FLAG_X;
          uint16_t result;
          int x = (cpu->sr & M68K_FLAG_X) ? 1 : 0;

          switch (typeE) {
            case 0: /* AS */
              if (leftE) {
                ccr = (val & 0x8000) ? (M68K_FLAG_X | M68K_FLAG_C) : 0;
                result = val << 1;
                if ((val ^ result) & 0x8000) ccr |= M68K_FLAG_V;
              } else {
                ccr = (val & 1) ? (M68K_FLAG_X | M68K_FLAG_C) : 0;
                result = (uint16_t)((int16_t)val >> 1);
              }
              break;
            case 1: /* LS */
              if (leftE) {
                ccr = (val & 0x8000) ? (M68K_FLAG_X | M68K_FLAG_C) : 0;
                result = val << 1;
              } else {
                ccr = (val & 1) ? (M68K_FLAG_X | M68K_FLAG_C) : 0;
                result = val >> 1;
              }
              break;
            case 2: /* ROX */
              if (leftE) {
                ccr = (val & 0x8000) ? (M68K_FLAG_X | M68K_FLAG_C) : 0;
                result = (val << 1) | x;
              } else {
                ccr = (val & 1) ? (M68K_FLAG_X | M68K_FLAG_C) : 0;
                result = (val >> 1) | (x << 15);
              }
              break;
            default: /* RO */
              if (leftE) {
                ccr = cpu->sr & M68K_FLAG_X; /* X unchanged */
                if (val & 0x8000) ccr |= M68K_FLAG_C;
                result = (val << 1) | (val >> 15);
              } else {
                ccr = cpu->sr & M68K_FLAG_X;
                if (val & 1) ccr |= M68K_FLAG_C;
                result = (val >> 1) | (val << 15);
              }
              break;
          }
          if (result == 0) ccr |= M68K_FLAG_Z;
          if (result & 0x8000) ccr |= M68K_FLAG_N;
          cpu->sr = (cpu->sr & 0xFF00) | ccr;
          m68k_write_ea(cpu, &eaE, M68K_SIZE_WORD, result);
        } else {
          /* Register shift: 1110 ccc d ss i tt rrr */
          int regE = opcode & 7;
          uint8_t typeE = (opcode >> 3) & 3;
          int irE = (opcode >> 5) & 1;
          int leftE = (opcode >> 8) & 1;
          int countE;
          if (irE)
            countE = cpu->d[(opcode >> 9) & 7] & 63;
          else {
            countE = (opcode >> 9) & 7;
            if (countE == 0) countE = 8;
          }

          uint32_t mask = m68k_size_mask(sizeE);
          uint32_t msb = m68k_size_msb(sizeE);
          uint32_t val = cpu->d[regE] & mask;
          uint32_t result = val;
          uint16_t ccr;
          int x = (cpu->sr & M68K_FLAG_X) ? 1 : 0;
          int bits = m68k_size_bytes(sizeE) * 8;

          if (countE == 0) {
            /* No shift: C=0 (except ROXL/R where C=X), X unchanged */
            ccr = cpu->sr & M68K_FLAG_X;
            if (typeE == 2) /* ROX: C = X */
              ccr |= x ? M68K_FLAG_C : 0;
            /* V=0 for all */
          } else {
            ccr = 0;
            switch (typeE) {
              case 0: /* AS */
                if (leftE) {
                  int v = 0;
                  for (int i = 0; i < countE; i++) {
                    int out = (result & msb) ? 1 : 0;
                    result = (result << 1) & mask;
                    if ((result & msb ? 1 : 0) != out) v = 1;
                    ccr = out ? (M68K_FLAG_X | M68K_FLAG_C) : 0;
                  }
                  if (v) ccr |= M68K_FLAG_V;
                } else {
                  int sign = (val & msb) ? 1 : 0;
                  for (int i = 0; i < countE; i++) {
                    ccr = (result & 1) ? (M68K_FLAG_X | M68K_FLAG_C) : 0;
                    result = (result >> 1) | (sign ? msb : 0);
                  }
                  result &= mask;
                }
                break;
              case 1: /* LS */
                if (leftE) {
                  for (int i = 0; i < countE; i++) {
                    ccr = (result & msb) ? (M68K_FLAG_X | M68K_FLAG_C) : 0;
                    result = (result << 1) & mask;
                  }
                } else {
                  for (int i = 0; i < countE; i++) {
                    ccr = (result & 1) ? (M68K_FLAG_X | M68K_FLAG_C) : 0;
                    result >>= 1;
                  }
                }
                break;
              case 2: /* ROX */
                for (int i = 0; i < countE; i++) {
                  if (leftE) {
                    int out = (result & msb) ? 1 : 0;
                    result = ((result << 1) & mask) | x;
                    x = out;
                  } else {
                    int out = result & 1;
                    result = (result >> 1) | (x ? msb : 0);
                    x = out;
                  }
                }
                ccr = x ? (M68K_FLAG_X | M68K_FLAG_C) : 0;
                break;
              default:                       /* RO */
                ccr = cpu->sr & M68K_FLAG_X; /* X unchanged */
                for (int i = 0; i < countE; i++) {
                  if (leftE) {
                    int out = (result & msb) ? 1 : 0;
                    result = ((result << 1) & mask) | out;
                  } else {
                    int out = result & 1;
                    result = (result >> 1) | (out ? msb : 0);
                  }
                }
                /* C = last bit rotated */
                if (leftE)
                  ccr = (cpu->sr & M68K_FLAG_X) |
                        ((result & 1) ? M68K_FLAG_C : 0);
                else
                  ccr = (cpu->sr & M68K_FLAG_X) |
                        ((result & msb) ? M68K_FLAG_C : 0);
                break;
            }
          }
          if (result == 0) ccr |= M68K_FLAG_Z;
          if (result & msb) ccr |= M68K_FLAG_N;
          cpu->sr = (cpu->sr & 0xFF00) | ccr;
          m68k_write_d(cpu, regE, result, sizeE);
          (void)bits;
        }
        break;
      }

      case 0xF: { /* F-line trap */
        int rc = m68k_fire_trap(cpu, CPU_TRAP_ILLEGAL, opcode);
        if (rc == CPU_TRAP_EXIT) return 0;
        break;
      }
    }

    if (cpu->step_budget) {
      cpu->step_budget--;
      if (!cpu->step_budget) return 0;
    }
  }
}

/* ── Common interface implementations ───────────────────────────────────── */

static void *ecpu_m68k_create_state() {
  proc_image_segment_t state_region;

  if (image_segment_alloc(&state_region, PPAP_MEM_RAM_DATA,
                          sizeof(m68k_state_t),
                          PROC_IMAGE_SEG_OWNED | PROC_IMAGE_SEG_WRITABLE) < 0)
    return NULL;
  return state_region.base;
}

static int ecpu_m68k_init(cpu_state_t *state, uint8_t *memory,
                          uint32_t mem_size) {
  m68k_state_t *cpu = (m68k_state_t *)state;
  memset(cpu, 0, sizeof(*cpu));
  cpu->memory = memory;
  cpu->mem_size = mem_size;
  /* Start in supervisor mode */
  cpu->sr = M68K_SR_S | 0x0700;
  return 0;
}

static void ecpu_m68k_set_trap_handler(cpu_state_t *state,
                                       cpu_trap_handler_t handler, void *ctx) {
  m68k_state_t *cpu = (m68k_state_t *)state;
  cpu->trap_handler = handler;
  cpu->trap_ctx = ctx;
}

static uint32_t ecpu_m68k_get_reg(cpu_state_t *state, int reg_id) {
  m68k_state_t *cpu = (m68k_state_t *)state;
  if (reg_id >= M68K_REG_D0 && reg_id <= M68K_REG_D7)
    return cpu->d[reg_id - M68K_REG_D0];
  if (reg_id >= M68K_REG_A0 && reg_id <= M68K_REG_A7)
    return cpu->a[reg_id - M68K_REG_A0];
  switch (reg_id) {
    case M68K_REG_PC:
      return cpu->pc;
    case M68K_REG_SR:
      return cpu->sr;
    case M68K_REG_USP:
      return cpu->usp;
    case M68K_REG_SSP:
      return cpu->ssp;
    default:
      return 0;
  }
}

static void ecpu_m68k_set_reg(cpu_state_t *state, int reg_id, uint32_t val) {
  m68k_state_t *cpu = (m68k_state_t *)state;
  if (reg_id >= M68K_REG_D0 && reg_id <= M68K_REG_D7) {
    cpu->d[reg_id - M68K_REG_D0] = val;
    return;
  }
  if (reg_id >= M68K_REG_A0 && reg_id <= M68K_REG_A7) {
    cpu->a[reg_id - M68K_REG_A0] = val;
    return;
  }
  switch (reg_id) {
    case M68K_REG_PC:
      cpu->pc = val;
      break;
    case M68K_REG_SR:
      cpu->sr = val & 0xFFFF;
      break;
    case M68K_REG_USP:
      cpu->usp = val;
      break;
    case M68K_REG_SSP:
      cpu->ssp = val;
      break;
  }
}

static void *ecpu_m68k_translate_ptr(cpu_state_t *state, uint32_t guest_addr,
                                     uint32_t size) {
  m68k_state_t *cpu = (m68k_state_t *)state;
  guest_addr &= cpu->mem_size - 1;
  if (guest_addr + size > cpu->mem_size) return 0;
  return cpu->memory + guest_addr;
}

static uint8_t ecpu_m68k_read8(cpu_state_t *state, uint32_t addr) {
  return m68k_read8((m68k_state_t *)state, addr);
}

static void ecpu_m68k_write8(cpu_state_t *state, uint32_t addr, uint8_t val) {
  m68k_write8((m68k_state_t *)state, addr, val);
}

static uint16_t ecpu_m68k_read16(cpu_state_t *state, uint32_t addr) {
  return m68k_read16((m68k_state_t *)state, addr);
}

static void ecpu_m68k_write16(cpu_state_t *state, uint32_t addr, uint16_t val) {
  m68k_write16((m68k_state_t *)state, addr, val);
}

static uint32_t ecpu_m68k_read32(cpu_state_t *state, uint32_t addr) {
  return ((uint32_t)m68k_read16((m68k_state_t *)state, addr) << 16) |
         m68k_read16((m68k_state_t *)state, addr + 2);
}

static void ecpu_m68k_write32(cpu_state_t *state, uint32_t addr, uint32_t val) {
  m68k_write16((m68k_state_t *)state, addr, val >> 16);
  m68k_write16((m68k_state_t *)state, addr + 2, val & 0xFFFF);
}

/* ── Core ops table ─────────────────────────────────────────────────────── */

const cpu_ops_t ecpu_m68k_ops = {
    .name = "m68k",
    .arch_id = CPU_ARCH_M68K,
    .flags = CPU_OPS_SEPARATE_USER_STACK,
    .create_state = ecpu_m68k_create_state,
    .init = (void *)ecpu_m68k_init,
    .run = (void *)ecpu_m68k_run,
    .set_trap_handler = (void *)ecpu_m68k_set_trap_handler,
    .get_reg = (void *)ecpu_m68k_get_reg,
    .set_reg = (void *)ecpu_m68k_set_reg,
    .translate_ptr = (void *)ecpu_m68k_translate_ptr,
    .read8 = (void *)ecpu_m68k_read8,
    .write8 = (void *)ecpu_m68k_write8,
    .read16 = (void *)ecpu_m68k_read16,
    .write16 = (void *)ecpu_m68k_write16,
    .read32 = (void *)ecpu_m68k_read32,
    .write32 = (void *)ecpu_m68k_write32,
};
