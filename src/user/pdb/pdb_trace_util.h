#ifndef PPAP_USER_PDB_PDB_TRACE_UTIL_H
#define PPAP_USER_PDB_PDB_TRACE_UTIL_H

#include "syscall.h"

int peek_u8(pid_t pid, uint32_t addr, uint8_t *byte_out);
int peek_u16le(pid_t pid, uint32_t addr, uint16_t *value_out);
int peek_u16be(pid_t pid, uint32_t addr, uint16_t *value_out);
int peek_u32be(pid_t pid, uint32_t addr, uint32_t *value_out);
int peek_u32le(pid_t pid, uint32_t addr, uint32_t *value_out);
int poke_u8(pid_t pid, uint32_t addr, uint8_t byte_value);

void print_mem_words(pid_t pid, uint32_t addr, uint32_t count);
void print_mem_bytes(pid_t pid, uint32_t addr, uint32_t count);
void print_mem_halfwords(pid_t pid, uint32_t addr, uint32_t count);

void disas_z80(pid_t pid, uint32_t pc, uint32_t count);
void disas_m68k(pid_t pid, uint32_t pc, uint32_t count);
void disas_thumb(pid_t pid, uint32_t pc, uint32_t count);

int z80_is_call_opcode(uint8_t op);
void print_backtrace(pid_t pid, const struct ppap_ptrace_regs *regs,
                     uint32_t count);

#endif /* PPAP_USER_PDB_PDB_TRACE_UTIL_H */
