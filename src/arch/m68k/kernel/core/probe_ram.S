| =============================================================================
| probe_ram.S — Runtime RAM size detection for m68k
| =============================================================================
|
| uint32_t m68k_probe_ram(uint32_t start, uint32_t max_size);
|
| Detects how much contiguous RAM is available starting at `start`, up to
| `max_size` bytes.  Probes in two phases:
|   Phase 1 (coarse): 1 MB steps to find the approximate ceiling
|   Phase 2 (fine):   4 KB (PAGE_SIZE) steps within the last MB
|
| Detection method: save the original value, write a unique pattern, read
| it back, verify it matches, then restore the original value.  This
| preserves memory contents (e.g. vector table in the first page).
|
| Works on both real hardware (where unmapped accesses may bus-error) and
| emulators (where unmapped reads may silently return 0).  A temporary bus
| error handler is installed to catch faults on hardware that asserts BERR.
|
| Each step tests the LAST longword of the chunk (chunk_start + STEP - 4),
| not the first.  Testing the first longword would falsely accept the whole
| chunk based on validity at its base; on XEiJ where physical RAM ends
| mid-chunk, that produced a page pool extending past real memory and a
| runtime bus error from page_write to the bogus last page.
|
| Must be called early at boot (supervisor mode, interrupts disabled).
| =============================================================================

    .set COARSE_STEP, 0x100000      | 1 MB
    .set FINE_STEP,   0x1000        | 4 KB (PAGE_SIZE)
    .set PATTERN,     0xA5F01234    | unique probe pattern

    .section .text, "ax"
    .align 2
    .global m68k_probe_ram

m68k_probe_ram:
    movem.l %d2-%d5/%a2-%a3,%sp@-   | save callee-saved (6 regs = 24 bytes)

    | Arguments (after 24 bytes of saved regs + 4 bytes return addr):
    |   28(%sp) = start address
    |   32(%sp) = max_size (bytes)
    move.l  28(%sp),%a2              | a2 = start
    move.l  32(%sp),%d2              | d2 = max_size

    | Save old bus error vector (vector 2 at address 0x8)
    move.l  0x8,%d3

    | d4 = probe pattern
    move.l  #PATTERN,%d4

    | ── Phase 1: coarse probe (1 MB steps) ──────────────────────────────────
    | Install coarse bus error handler
    lea.l   .Lcoarse_buserr(%pc),%a0
    move.l  %a0,0x8

    move.l  %a2,%a3                  | a3 = current probe address
    moveq   #0,%d0                   | d0 = confirmed RAM size (bytes)

.Lcoarse_loop:
    cmp.l   %d2,%d0
    bge.s   .Ldone                   | checked all max_size → done
    | Probe the last longword of the chunk so the chunk's claim of
    | validity actually covers its full range.  68000 lea.l displacement
    | is 16-bit, so use add.l for the 1 MB offset.
    move.l  %a3,%a0
    add.l   #COARSE_STEP-4,%a0
    move.l  (%a0),%d5               | d5 = save original (may bus error)
    move.l  %d4,(%a0)               | write pattern
    cmp.l   (%a0),%d4               | read back and compare
    bne.s   .Lcoarse_restore_fail   | mismatch → not real RAM
    move.l  %d5,(%a0)               | restore original value
    | Read succeeded and matched — this MB is accessible
    add.l   #COARSE_STEP,%d0
    add.l   #COARSE_STEP,%a3
    bra.s   .Lcoarse_loop

.Lcoarse_restore_fail:
    | Pattern mismatch — not real memory (don't bother restoring)
    bra.s   .Lfine_start

    | Bus error during coarse probe:
    | CPU pushed 14-byte group-0 exception frame.
    | Discard it and proceed to fine phase.
    | d0 = last confirmed size (not incremented for the failed access).
.Lcoarse_buserr:
    lea.l   14(%sp),%sp              | discard exception frame

    | ── Phase 2: fine probe (4 KB steps) ────────────────────────────────────
    | Probe per-page from the coarse boundary to find exact RAM end.
.Lfine_start:
    | Install fine bus error handler
    lea.l   .Lfine_buserr(%pc),%a0
    move.l  %a0,0x8

    move.l  %a2,%a3                  | a3 = start + d0
    add.l   %d0,%a3

.Lfine_loop:
    cmp.l   %d2,%d0
    bge.s   .Ldone                   | checked all max_size → done
    | Probe last longword of this 4 KB page (see coarse_loop comment).
    lea.l   FINE_STEP-4(%a3),%a0
    move.l  (%a0),%d5               | d5 = save original (may bus error)
    move.l  %d4,(%a0)               | write pattern
    cmp.l   (%a0),%d4               | read back and compare
    bne.s   .Ldone                  | mismatch → end of RAM
    move.l  %d5,(%a0)               | restore original value
    | Page is accessible
    add.l   #FINE_STEP,%d0
    add.l   #FINE_STEP,%a3
    bra.s   .Lfine_loop

    | Bus error during fine probe:
    | d0 = exact RAM size (not incremented for the failed access).
.Lfine_buserr:
    lea.l   14(%sp),%sp              | discard exception frame

.Ldone:
    | Restore old bus error vector
    move.l  %d3,0x8

    | Return d0 = total accessible RAM from start (bytes)
    movem.l %sp@+,%d2-%d5/%a2-%a3   | restore callee-saved
    rts
