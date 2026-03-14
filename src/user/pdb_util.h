#ifndef PPAP_USER_PDB_UTIL_H
#define PPAP_USER_PDB_UTIL_H

#include "syscall.h"

void put_str(const char *s);
void put_err(const char *s);
void put_chr(char c);
void put_u32(uint32_t v);
void put_i32(int32_t v);
void put_hex32(uint32_t v);
void put_hex16(uint32_t v);
void put_hex8(uint32_t v);

uint32_t select_bp_flag_from_caps(uint32_t caps_bits);
const char *bp_flag_name(uint32_t flags);

int streq(const char *a, const char *b);
int readline(char *buf, int size);
int split_tokens(char *line, char **tok, int max_tok);
int is_script_space(char c);

int load_script_file(const char *path, char **script_cmds,
                     int *script_count, char *storage,
                     int *storage_used);

int parse_u32(const char *s, uint32_t *out);
int parse_x_spec(const char *tok0, uint32_t *count_out, char *fmt_out);
int parse_mem_width(const char *tok, uint32_t *width_out);
int parse_surface_token(const char *token, uint32_t *surface_out);

#endif /* PPAP_USER_PDB_UTIL_H */
