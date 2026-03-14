| =============================================================================
| bootldr.S — PPAP X68000 single-stage boot loader (1 sector = 1024 bytes)
| =============================================================================
|
| Interrupt strategy:
|   - During floppy reads: interrupts ENABLED — _B_READ waits for FDC interrupt;
|     masking (IPM=7) causes it to hang.  IPL ROM vectors at 0x000000 are valid.
|   - Before/during vector copy: IPM=7 — prevents hardware interrupts from hitting
|     the partially-installed kernel vector table before kernel init.
|
| IOCS _B_READ (function 0x46) — CHS addressing:
|   d0.l = 0x46
|   d1.w = (PDA << 8) | mode   (.hb = high byte of the word = bits 15-8)
|          PDA  0x90–0x93 = 2HD FDD 0–3  (use 0x90 for FDD0)
|          mode bit6=MFM, bit5=retry, bit4=seek  (use 0x70)
|   d2.l = (sector_length << 24) | (cylinder << 16) | (head << 8) | sector
|          sector_length 3 = 1024 B; sector is 1-based (1–8)
|   d3.l = byte count  (sectors × 1024)
|   a1.l = buffer
|   Returns d0: FDC status (ST0|ST1|ST2); 0 = success
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
    | Do NOT mask interrupts here — _B_READ waits for an FDC interrupt;
    | with IPM=7 it would hang.  The IPL ROM's vectors at 0x000000 are still
    | valid, so hardware interrupts are safe until we overwrite the table.
    lea.l   0x002000,%sp

    | Save IPL IOCS vector: TRAP #15 = vector 47 → address 47×4 = 0x0000BC.
    | Keep in a5 throughout (a5 is preserved by _B_READ per IOCS convention).
    move.l  0x0000BC,%a5

    | ── CHS read loop — 1 sector at a time (matches Human68k boot approach) ──
    | X68000 2HD geometry: 16 sectors/cylinder (2 heads × 8 sectors)
    |   cylinder     = lsec / 16
    |   head         = (lsec % 16) >> 3   (bit 3 of within_cyl)
    |   sector       = (lsec % 16) & 7 + 1  (1-based, 1..8)
    |
    | Loop registers (preserved across _B_READ: d4–d7, a2–a7):
    |   D5.w  = remaining sectors
    |   D6.l  = current logical sector (starts at 1)
    |   A3.l  = current buffer pointer  (starts at 0x006000)
    |   A5.l  = saved IPL IOCS vector   (for error recovery)
    | ────────────────────────────────────────────────────────────────────────
    move.w  #KERNEL_SECTORS,%d5     | remaining (literal patched by mkx68kimg.sh)
    clr.l   %d6
    move.w  #1,%d6                  | start at logical sector 1 (skip boot sector)
    lea.l   0x006000,%a3            | kernel load destination

read_loop:
    | LBA → CHS for current sector (D6)
    move.l  %d6,%d0
    divu.w  #16,%d0                 | d0.w = cylinder; upper(d0) = within_cyl (0..15)
    move.w  %d0,%d7                 | d7.w = cylinder (d7 preserved by IOCS)

    swap    %d0                     | d0.w = within_cyl (0..15)
    move.w  %d0,%d4
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
    clr.l   %d1                     | clear leftover cylinder bits before head
    move.b  %d0,%d1
    lsl.l   #8,%d1                  | d1 = head << 8
    or.l    %d1,%d2
    or.b    %d4,%d2                 | d2 |= sector

    | call _B_READ (0x46): read exactly 1 sector (1024 bytes)
    move.l  %a3,%a1                 | buffer
    move.l  #1024,%d3               | byte count = 1 sector
    move.w  #0x9070,%d1             | PDA=0x90 (2HD FDD0) in bits 15-8, mode=0x70 in bits 7-0
    move.l  #0x46,%d0
    trap    #15
    btst    #30,%d0                 | IC bit 6 set = abnormal termination / invalid
    bne.s   read_error

    | advance to next sector
    addq.w  #1,%d6                  | lsec++
    subq.w  #1,%d5                  | remaining--
    adda.l  #1024,%a3               | buffer += 1024
    tst.w   %d5
    bne.s   read_loop

    | ── Read loop complete ───────────────────────────────────────────────────
    | Kernel presence check: first longword at 0x006000 should be
    | 0x00006400 (= __stack_top = Initial SSP in the .vectors section).
    cmp.l   #0x00006400,0x006000
    beq.s   .kernel_ok
    | Kernel not present — wrong read (function code? drive? sector numbering?)
    bra.w   .halted
.kernel_ok:
    | ── Mask all interrupts before overwriting the vector table ─────────────
    | From this point the IPL ROM's handlers are gone; mask until the kernel
    | initialises its own handlers.
    or.w    #0x0700,%sr

    | ── Copy kernel .vectors (1024 bytes: 0x006000 → 0x000000) ─────────────
    lea.l   0x006000,%a0
    movea.l #0,%a1
    move.w  #(256-1),%d0
copy_loop:
    move.l  (%a0)+,(%a1)+
    dbra    %d0,copy_loop

    | Restore TRAP #15 to IPL IOCS handler for kernel uart_x68k.c to use.
    move.l  %a5,0x0000BC

    | Jump to kernel Reset_Handler at 0x006400.
    jmp     0x006400

    | ── _B_READ error handler ────────────────────────────────────────────────
    | FDC status: d0.l = (ST0<<24)|(ST1<<16)|(ST2<<8)|cylinder
    | Print "Exxxxxxxx" — all 8 hex digits MSB first.
read_error:
    move.l  %d0,%d7                 | save error code (d7 preserved by IOCS)
    move.l  %a5,0x0000BC            | restore valid IOCS vector before printing

    move.l  #0x20,%d0
    move.l  #'E',%d1
    trap    #15

    | Loop: print 8 nibbles from bit 28 down to bit 0
    move.l  #28,%d5                 | shift count (reuse d5; we're done with loop)
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
