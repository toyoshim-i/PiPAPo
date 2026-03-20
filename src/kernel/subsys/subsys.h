/*
 * subsys.h — OS personality (subsystem) abstraction layer
 *
 * The kernel calls through subsys_ops hooks instead of hardcoding
 * subsystem-specific logic.  Each OS personality (e.g. Human68k)
 * provides its own ops struct registered via subsys_ops_table[tag].
 *
 * This keeps the kernel/subsystem layering boundary clear:
 *   - proc.h has a generic `subsys` tag + opaque `subsys_data` pointer
 *   - Kernel code (crash handler, signal delivery) calls subsys hooks
 *   - Subsystem code (human68k_bridge.c) implements the hooks
 */

#ifndef PPAP_SUBSYS_SUBSYS_H
#define PPAP_SUBSYS_SUBSYS_H

#include <stdint.h>

/* Forward declarations */
struct pcb;

/*
 * subsys_ops — per-personality hook table.
 *
 * All callbacks are optional (NULL = use default kernel behavior).
 * They are called from kernel code at the appropriate points.
 */
typedef struct subsys_ops {
    /*
     * on_crash — called when the process faults (bus error, illegal
     *            instruction, etc.).
     *
     * @p:     faulting process
     * @regs:  saved register frame
     * @exc:   exception frame (SR + PC, or group-0 14-byte frame)
     * @is_group0: 1 if bus/address error (14-byte frame), 0 otherwise
     *
     * Returns: 0 = not handled (kernel uses default behavior)
     *          1 = process killed (caller reschedules)
     *          2 = handled (restore regs and RTE)
     */
    int (*on_crash)(struct pcb *p, uint32_t *regs, uint16_t *exc,
                    int is_group0);

    /*
     * on_signal — called when a signal with SIG_DFL disposition is
     *             about to terminate the process.
     *
     * @p:     target process
     * @sig:   signal number
     * @regs:  saved register frame (m68k only, NULL on ARM)
     *
     * Returns: 0 = not handled (kernel uses default: sys_exit(128+sig))
     *          1 = handled (subsystem took action, e.g. redirected PC)
     */
    int (*on_signal)(struct pcb *p, int sig, uint32_t *regs);

    /*
     * on_init — called after exec to initialize subsystem-specific
     *           per-process state.
     *
     * @p:     newly exec'd process
     */
    void (*on_init)(struct pcb *p);

    /*
     * on_exit — called during sys_exit before fds are closed.
     *
     * Use this to restore terminal state or release subsystem resources
     * that depend on open file descriptors.
     *
     * @p:     exiting process
     */
    void (*on_exit)(struct pcb *p);

    /*
     * on_proc_read — generate content for /proc/<pid>/termconv.
     *
     * @p:     target process
     * @name:  filename being read (e.g. "termconv")
     * @buf:   output buffer
     * @bufsiz: buffer size
     *
     * Returns: number of bytes written, or 0 if the file is not provided
     *          by this subsystem.
     */
    int (*on_proc_read)(struct pcb *p, const char *name,
                        char *buf, int bufsiz);
} subsys_ops_t;

/* Maximum number of subsystem types (index into subsys_ops_table[]) */
#define SUBSYS_MAX  5

/* Global ops table — indexed by pcb_t::subsys tag.
 * Slot 0 (SUBSYS_PPAP) is NULL (default kernel behavior). */
extern const subsys_ops_t *subsys_ops_table[SUBSYS_MAX];

/* Register subsystem names with procfs. Call once at boot. */
void subsys_init(void);

#endif /* PPAP_SUBSYS_SUBSYS_H */
