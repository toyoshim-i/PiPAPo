#ifndef PPAP_USER_PDB_PDB_UTIL_H
#define PPAP_USER_PDB_PDB_UTIL_H

#include "lib/uclib.h"

/* Output helpers — thin redirects to uclib. */
#define put_str uc_puts
#define put_err uc_eputs
#define put_chr uc_putc
#define put_u32 uc_putu
#define put_i32 uc_puti
#define put_hex32 uc_putx32
#define put_hex16 uc_putx16
#define put_hex8 uc_putx8

static inline int streq(const char *a, const char *b) {
  return uc_strcmp(a, b) == 0;
}

/* pdb-specific helpers */
uint32_t select_bp_flag_from_caps(uint32_t caps_bits);
const char *bp_flag_name(uint32_t flags);

int readline(char *buf, int size);
int split_tokens(char *line, char **tok, int max_tok);
int is_script_space(char c);

int load_script_file(const char *path, char **script_cmds, int *script_count,
                     char *storage, int *storage_used);

int parse_u32(const char *s, uint32_t *out);
int parse_x_spec(const char *tok0, uint32_t *count_out, char *fmt_out);
int parse_mem_width(const char *tok, uint32_t *width_out);
int parse_surface_token(const char *token, uint32_t *surface_out);

#endif /* PPAP_USER_PDB_PDB_UTIL_H */
