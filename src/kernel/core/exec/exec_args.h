/*
 * exec_args.h — Captured execve argument payload
 *
 * Holds the (path, argv[], envp[]) triple copied out of user memory
 * during sys_execve, in a single data-region page accessed exclusively
 * via page_read/write.  Using the page API lets the same
 * code run on 32-bit arches (where the page lives in kernel-addressable
 * RAM) and on ia16 (where pool pages sit above 64 KB and require
 * segment-setup under the hood).
 *
 * Page layout (all offsets compile-time constants):
 *
 *   [EXEC_ARGS_PATH_OFF      .. +VFS_PATH_MAX)    path[] (NUL-term)
 *   [EXEC_ARGS_ARGV_TBL_OFF  .. +N*2)             uint16_t argv_off[N]
 *                                                 (N = EXEC_ARGV_MAX + 1)
 *   [EXEC_ARGS_ENVP_TBL_OFF  .. +M*2)             uint16_t envp_off[M]
 *                                                 (M = EXEC_ENVP_MAX + 1)
 *   [EXEC_ARGS_ARGV_BUF_OFF  .. +EXEC_ARGV_BYTES_MAX)  argv string bytes
 *   [EXEC_ARGS_ENVP_BUF_OFF  .. +EXEC_ENVP_BYTES_MAX)  envp string bytes
 *
 * argv_off[i] is the offset (within the page) of the i-th NUL-terminated
 * argv string in argv_buf.  Entry argv_off[argc] is the one-past-end
 * offset, so the length of argv[i] is argv_off[i+1] - argv_off[i] - 1.
 * envp follows the same convention.
 */

#ifndef PPAP_KERNEL_CORE_EXEC_EXEC_ARGS_H
#define PPAP_KERNEL_CORE_EXEC_EXEC_ARGS_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/common/config.h"
#include "kernel/common/core/page_types.h"
#include "kernel/core/exec/exec.h"

/* ── Compile-time page layout ─────────────────────────────────────────── */

#define EXEC_ARGS_PATH_OFF 0u

#define EXEC_ARGS_ARGV_TBL_OFF ((uint16_t)VFS_PATH_MAX)

#define EXEC_ARGS_ARGV_TBL_BYTES \
  ((uint16_t)((EXEC_ARGV_MAX + 1) * sizeof(uint16_t)))

#define EXEC_ARGS_ENVP_TBL_OFF \
  ((uint16_t)(EXEC_ARGS_ARGV_TBL_OFF + EXEC_ARGS_ARGV_TBL_BYTES))

#define EXEC_ARGS_ENVP_TBL_BYTES \
  ((uint16_t)((EXEC_ENVP_MAX + 1) * sizeof(uint16_t)))

#define EXEC_ARGS_ARGV_BUF_OFF \
  ((uint16_t)(EXEC_ARGS_ENVP_TBL_OFF + EXEC_ARGS_ENVP_TBL_BYTES))

#define EXEC_ARGS_ENVP_BUF_OFF \
  ((uint16_t)(EXEC_ARGS_ARGV_BUF_OFF + EXEC_ARGV_BYTES_MAX))

#define EXEC_ARGS_TOTAL_SIZE \
  ((uint16_t)(EXEC_ARGS_ENVP_BUF_OFF + EXEC_ENVP_BYTES_MAX))

_Static_assert(EXEC_ARGS_TOTAL_SIZE <= PAGE_SIZE,
               "exec_args layout exceeds one page");

/* ── Handle ───────────────────────────────────────────────────────────── */

typedef struct exec_args {
  page_id_t page; /* backing page (PAGE_ID_INVALID when empty)      */
  uint8_t argc;   /* number of argv entries (0..EXEC_ARGV_MAX)      */
  uint8_t envc;   /* number of envp entries (0..EXEC_ENVP_MAX)      */
} exec_args_t;

/* ── Builder (populate) ───────────────────────────────────────────────── */

/*
 * Initialize a fresh exec_args_t bound to `page`.  Zeroes the header
 * tables; argv_off[0] / envp_off[0] are set to the start of their
 * respective buffer so the first append lands at the buffer base.
 */
void exec_args_init(exec_args_t *a, page_id_t page);

/*
 * Write `path` into the args page.  `path` must be a NUL-terminated
 * kernel-addressable string.  Returns 0 on success, -ENAMETOOLONG if
 * the string (including NUL) exceeds VFS_PATH_MAX.
 */
int exec_args_set_path(exec_args_t *a, const char *path);

/*
 * Append an argv string of length `len` (excluding NUL) from kernel
 * memory.  Returns 0 on success, -E2BIG on vector-slot or byte-budget
 * overflow.
 */
int exec_args_append_argv(exec_args_t *a, const char *str, uint16_t len);

/*
 * Append an envp string.  Semantics mirror exec_args_append_argv.
 */
int exec_args_append_envp(exec_args_t *a, const char *str, uint16_t len);

/*
 * Streaming append — used when the caller wants to write the string
 * bytes directly into the args page (e.g. via sys_copy_user_string_to_page)
 * without first staging them in a kernel buffer.
 *
 * exec_args_argv_begin reserves the next argv slot and returns the
 * (page, offset) where the bytes should be written, plus the maximum
 * number of bytes that fit (excluding the eventual NUL terminator).
 * Returns -E2BIG if no slot is left or no space remains.
 *
 * After writing exactly `len` bytes (no NUL) at the returned offset,
 * the caller invokes exec_args_argv_commit(a, len) to write the NUL
 * and finalize the offset table.
 */
int exec_args_argv_begin(exec_args_t *a, page_id_t *out_page, uint16_t *out_off,
                         uint16_t *out_max_len);
int exec_args_argv_commit(exec_args_t *a, uint16_t len);

int exec_args_envp_begin(exec_args_t *a, page_id_t *out_page, uint16_t *out_off,
                         uint16_t *out_max_len);
int exec_args_envp_commit(exec_args_t *a, uint16_t len);

/* ── Reader (consume) ─────────────────────────────────────────────────── */

/*
 * Copy the NUL-terminated path out of the args page into `out`.
 * Returns the string length (excluding NUL) on success, -ENAMETOOLONG
 * if `out_size` is too small.
 */
int exec_args_path(const exec_args_t *a, char *out, uint16_t out_size);

/*
 * Length of the i-th argv string (excluding NUL).  Returns 0 if `idx`
 * is out of range (caller should bound-check against `a->argc`).
 */
uint16_t exec_args_argv_len(const exec_args_t *a, int idx);
uint16_t exec_args_envp_len(const exec_args_t *a, int idx);

/*
 * Read a single byte at `byte_off` within the i-th argv/envp string.
 * Lets callers scan a string without staging the whole thing in a
 * kernel buffer.  Returns 0 on success (byte stored in *out), -1 if
 * `idx` or `byte_off` is out of range.
 */
int exec_args_argv_byte(const exec_args_t *a, int idx, uint16_t byte_off,
                        char *out);
int exec_args_envp_byte(const exec_args_t *a, int idx, uint16_t byte_off,
                        char *out);

/*
 * Copy the i-th argv string into `out` (NUL-terminated).  Returns the
 * length copied (excluding NUL), or -ENAMETOOLONG if `out_size` is
 * smaller than needed, or -1 if `idx` is out of range.
 */
int exec_args_argv_copy(const exec_args_t *a, int idx, char *out,
                        uint16_t out_size);
int exec_args_envp_copy(const exec_args_t *a, int idx, char *out,
                        uint16_t out_size);

/*
 * Stream the i-th argv/envp string (excluding NUL) byte-by-byte from
 * the args page directly into a destination memory range starting at
 * `base_page` + `start_off` linear bytes.  The helper handles page
 * boundary crossing internally so a single string can span multiple
 * destination pages.  No kernel staging buffer is used — keeps the
 * exec call chain's stack footprint flat (important on ia16 where the
 * kernel stack is 1 KB).  Returns the number of bytes written
 * (excluding NUL), or -1 if `idx` is out of range.
 */
int exec_args_argv_to_page(const exec_args_t *a, int idx, page_id_t base_page,
                           uint32_t start_off);
int exec_args_envp_to_page(const exec_args_t *a, int idx, page_id_t base_page,
                           uint32_t start_off);

/*
 * Stream the path field byte-by-byte from the args page into the same
 * (base_page, start_off) destination model as the argv/envp variants,
 * excluding the terminating NUL.  At most `dst_max` bytes are written.
 * Used for intra-args-page synthesis (default argv[0] = path) and for
 * cases where a loader places the path verbatim into a process image.
 * Returns the number of bytes written, or -ENAMETOOLONG if `dst_max`
 * is exhausted before NUL.
 */
int exec_args_path_to_page(const exec_args_t *a, page_id_t base_page,
                           uint32_t start_off, uint16_t dst_max);

#endif /* PPAP_KERNEL_CORE_EXEC_EXEC_ARGS_H */
