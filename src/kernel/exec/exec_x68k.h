/*
 * exec_x68k.h — Human68k X-format binary loader
 *
 * Detects and loads Human68k X-format (.x) executables.  On native m68k,
 * the binary runs directly with F-line exceptions intercepted by the kernel
 * for DOS call translation.
 */

#ifndef PPAP_EXEC_EXEC_X68K_H
#define PPAP_EXEC_EXEC_X68K_H

#include "kernel/proc/proc.h"
#include <stdint.h>

/* X-format magic: "HU" (0x48 0x55) at offset 0 */
#define X68K_MAGIC_0  0x48
#define X68K_MAGIC_1  0x55

/*
 * x68k_detect — check if file starts with the "HU" magic.
 *
 * Returns 1 if the file is an X-format Human68k binary, 0 otherwise.
 */
int x68k_detect(const uint8_t *file, uint32_t size);

/*
 * exec_x68k — load and set up an X-format binary for execution.
 *
 * Called from do_execve() when x68k_detect() succeeds.
 * On success returns 0; the PCB is ready and the caller sets RUNNABLE.
 * On failure returns negative errno.
 */
int exec_x68k(pcb_t *p, const uint8_t *file, uint32_t size,
              const char *path, const char *const *argv);

#endif /* PPAP_EXEC_EXEC_X68K_H */
