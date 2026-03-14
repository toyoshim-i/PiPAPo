| =============================================================================
| stage1.S — PPAP X68000 Stage 1 Boot Sector (sector 0, 1024 bytes max)
| =============================================================================
|
| Loaded by the X68000 IPL ROM to 0x002000.  Reads stage2 (sectors 1–3,
| 3072 bytes) to 0x003000, saves the IPL IOCS vector, and jumps to stage2.
|
| Stage2 (UFS kernel loader) then:
|   - Parses the UFS filesystem on the floppy (from sector 4 onward)
|   - Loads boot/kernel to 0x006000
|   - Loads boot/rootfs.ufs to RAM after the kernel
|   - Writes rootfs address/size to 0x002FF8/0x002FFC
|   - Patches the CPU vector table, restores TRAP #15, jumps to 0x006400
|
| IOCS _B_READ (function 0x46) — CHS addressing:
|   d0.l = 0x46
|   d1.w = (PDA << 8) | mode
|          PDA  0x90 = 2HD FDD0; mode 0x70 = MFM | retry | seek
|   d2.l = (sector_length << 24) | (cylinder << 16) | (head << 8) | sector
|          sector_length 3 = 1024 B; sector is 1-based (1–8)
|   d3.l = byte count  (1024 per sector)
|   a1.l = buffer pointer
|   Returns d0: FDC status (0 = success)
|
| Preserved across _B_READ: d4–d7, a2–a7
| =============================================================================

    .text
    .align 2

    | --- IPL boot signature: BRA.S past the 34-byte DPB ---
    bra.s   boot_start

    | --- Disk Parameter Block (offsets 2–35, 34 bytes) ---
    .ascii  "PPAP    "              | offsets  2– 9: OEM name (8 bytes)
    .byte   0x00, 0x04              | offsets 10–11: bytes_per_sector = 1024 (LE)
    .byte   0x02                    | offset  12:    sectors_per_cluster = 2
    .byte   0x01, 0x00              | offsets 13–14: reserved_sectors = 1 (LE)
    .byte   0x02                    | offset  15:    num_fats = 2
    .byte   0x40, 0x00              | offsets 16–17: root_dir_entries = 64 (LE)
    .byte   0xD0, 0x04              | offsets 18–19: total_sectors = 1232 (LE)
    .byte   0xFE                    | offset  20:    media_descriptor (2HD)
    .byte   0x06, 0x00              | offsets 21–22: sectors_per_fat = 6 (LE)
    .byte   0x08, 0x00              | offsets 23–24: sectors_per_track = 8 (LE)
    .byte   0x02, 0x00              | offsets 25–26: num_heads = 2 (LE)
    .byte   0x00, 0x00, 0x00, 0x00  | offsets 27–30: hidden_sectors = 0
    .byte   0x00, 0x00, 0x00, 0x00, 0x00  | offsets 31–35: padding

boot_start:
    | Set temporary stack (grows downward from 0x002000 into free low RAM).
    | Interrupts must remain ENABLED — _B_READ waits for FDC interrupts.
    lea.l   0x002000,%sp

    | Save IPL IOCS vector: TRAP #15 = vector 47 → 47×4 = 0x0000BC.
    | Keep in d5 during the loop (preserved by _B_READ).
    move.l  0x0000BC,%d5

    | ── Read 3 sectors (LBA 1–3) to 0x003000 ───────────────────────────────
    | X68000 2HD geometry: 16 sectors/cylinder (2 heads × 8 sectors)
    |   cylinder     = lsec / 16
    |   head         = (lsec % 16) >> 3
    |   sector       = (lsec % 16) & 7 + 1  (1-based, 1..8)
    |
    | Loop registers (preserved across _B_READ):
    |   D4.w  = remaining sector count
    |   D5.l  = saved IPL IOCS vector
    |   D6.l  = current logical sector (starts at 1)
    |   D7.w  = cylinder (temporary, also preserved)
    |   A3.l  = current buffer pointer (starts at 0x003000)
    | ─────────────────────────────────────────────────────────────────────────
    move.w  #3,%d4                  | read 3 sectors (stage2 = sectors 1,2,3)
    clr.l   %d6
    move.w  #1,%d6                  | start at logical sector 1
    lea.l   0x003000,%a3            | stage2 load destination

read_loop:
    | LBA → CHS for current sector (D6)
    move.l  %d6,%d0
    divu.w  #16,%d0                 | d0.w = cylinder; upper(d0) = within_cyl
    move.w  %d0,%d7                 | d7.w = cylinder

    swap    %d0                     | d0.w = within_cyl (0..15)
    move.w  %d0,%d4                 | save within_cyl in d4 temporarily
    and.w   #7,%d4
    addq.w  #1,%d4                  | d4.w = sector (1-based, 1..8)
    lsr.w   #3,%d0                  | d0.w = head (0 or 1)

    | Build d2.l = (sector_length << 24) | (cylinder << 16) | (head << 8) | sector
    move.l  #0x03000000,%d2         | sector_length = 3 (1024 B)
    clr.l   %d1
    move.b  %d7,%d1
    lsl.l   #8,%d1
    lsl.l   #8,%d1                  | d1 = cylinder << 16
    or.l    %d1,%d2
    clr.l   %d1
    move.b  %d0,%d1
    lsl.l   #8,%d1                  | d1 = head << 8
    or.l    %d1,%d2
    or.b    %d4,%d2                 | d2 |= sector (1-based)

    | Read 1 sector (1024 bytes) via IOCS _B_READ
    | d4 is clobbered here; we restore remaining count below
    move.l  %a3,%a1                 | buffer
    move.l  #1024,%d3               | byte count
    move.w  #0x9070,%d1             | PDA=0x90 (2HD FDD0), mode=0x70
    move.l  #0x46,%d0
    trap    #15
    tst.l   %d0
    bne.w   read_error

    | Advance: sector++, remaining--, buf += 1024
    addq.l  #1,%d6                  | lsec++
    | restore remaining count: we overwrote d4 during CHS calc — recompute
    | from d6: sectors_done = d6 - 1 - 1 (started at 1, done after addq)
    | Simpler: track remaining in a separate way
    | Actually, since we clobber d4 in the CHS calc, let's recompute it.
    | We read sectors 1, 2, 3.  We're done when d6 == 4.
    adda.l  #1024,%a3               | buffer += 1024
    cmpi.w  #4,%d6                  | done when next lsec == 4
    bne.s   read_loop

    | ── Stage2 loaded at 0x003000 ──────────────────────────────────────────
    | Store saved IOCS vector to fixed address for stage2 to restore later.
    move.l  %d5,0x002FF0

    | Jump to stage2 entry point
    jmp     0x003000

    | ── _B_READ error handler ───────────────────────────────────────────────
read_error:
    move.l  %d0,%d7                 | save error code
    move.l  %d5,0x0000BC            | restore valid IOCS before printing

    move.l  #0x20,%d0               | _B_PUTC
    move.l  #'E',%d1
    trap    #15

    move.l  #28,%d5                 | shift: 28 down to 0 in steps of 4
.hex_loop:
    move.l  %d7,%d0
    lsr.l   %d5,%d0
    and.l   #0x0F,%d0
    add.l   #'0',%d0
    cmp.l   #'9'+1,%d0
    blo.s   .nibble_ok
    addq.l  #7,%d0
.nibble_ok:
    move.l  %d0,%d1
    move.l  #0x20,%d0
    trap    #15
    subq.l  #4,%d5
    bge.s   .hex_loop

.halted:
    stop    #0x2700
    bra.s   .halted
