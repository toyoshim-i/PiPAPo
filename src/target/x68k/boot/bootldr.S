| =============================================================================
| bootldr.S — PPAP X68000 single-stage boot loader (1 sector = 1024 bytes)
| =============================================================================
|
| The X68000 IPL ROM reads the first 1024-byte sector from the boot floppy
| into memory at 0x002000 and jumps there if the first byte is 0x60 (BRA.S).
|
| This loader:
|   1. Saves IPL IOCS vector (TRAP #15, entry 47 at 0x0000BC)
|   2. Reads the raw kernel binary (sectors 1..N) from floppy to 0x006000
|      using IOCS _B_READ (function code 0x04)
|   3. Copies kernel .vectors (first 1024 bytes at 0x006000) to 0x000000,
|      replacing IPL vectors with PPAP kernel handlers
|   4. Restores TRAP #15 at 0x0000BC to saved IPL IOCS handler, so the
|      kernel can continue using IOCS calls (uart_x68k.c)
|   5. Jumps to Reset_Handler at 0x006400
|
| Floppy layout (X68000 2HD: 77 tracks × 2 heads × 8 sectors × 1024 B):
|   Sector 0 (1024 B): This boot loader
|   Sector 1–N (N×1024 B): Raw kernel binary (ppap_x68k.bin)
|   Sector N+1 onward: Unused (zeros)
|
| IOCS _B_READ parameters (d0=0x04):
|   d1.l = drive number (0 = FDD0 / drive A)
|   d2.l = first logical sector (0-based; 0 = boot sector, 1 = sector after it)
|   d3.l = number of sectors to read
|   a1.l = destination buffer address
|   Returns d0.l = 0 on success, error code on failure
| =============================================================================

    .text
    .align 2

    | --- IPL boot signature: first byte MUST be 0x60 (BRA.S) ---
    bra.s   boot_start
    .ascii  "PPAP-BOOT "        | 10-byte volume label (offsets 2–11)

    | --- Boot code ---
boot_start:
    | Set temporary stack; grows downward from 0x002000 into free low RAM.
    | The area 0x000400–0x001FFF is free at this point.
    lea.l   0x002000,%sp

    | Save the IPL IOCS handler address before we overwrite the vector table.
    | TRAP #15 = vector 47 → offset 47 × 4 = 0xBC in vector table at 0x000000.
    move.l  0x0000BC,%a2

    | Load kernel binary from floppy sectors 1..N into 0x006000.
    | KERNEL_SECTORS is the literal value filled in by mkx68kimg.sh via sed.
    move.l  #0x04,%d0           | _B_READ function code
    clr.l   %d1                 | drive 0 (FDD0 = drive A)
    move.l  #1,%d2              | start at logical sector 1 (skip boot sector 0)
    move.l  #KERNEL_SECTORS,%d3 | number of sectors (patched by mkx68kimg.sh)
    lea.l   0x006000,%a1        | load destination
    trap    #15
    tst.l   %d0
    bne.s   read_error

    | Copy kernel .vectors (first 1024 bytes of kernel at 0x006000) to 0x000000.
    | This installs PPAP kernel exception/interrupt handlers in the CPU table.
    lea.l   0x006000,%a0
    movea.l #0,%a1
    move.w  #(256-1),%d0        | 256 longwords = 1024 bytes
copy_loop:
    move.l  (%a0)+,(%a1)+
    dbra    %d0,copy_loop

    | Restore TRAP #15 (vector 47) to the saved IPL IOCS handler so the
    | kernel can call IOCS functions (_B_PUTC, _B_GETC, etc.) via trap #15.
    move.l  %a2,0x0000BC

    | Jump to Reset_Handler; it will re-set SP = __stack_top = 0x006400,
    | copy .data, zero .bss, and call kmain().
    jmp     0x006400

read_error:
    | Display "ERR" on TVRAM console via IOCS _B_PUTC (0x20) and halt.
    move.l  #0x20,%d0
    move.l  #69,%d1             | 'E'
    trap    #15
    move.l  #0x20,%d0
    move.l  #82,%d1             | 'R'
    trap    #15
    move.l  #0x20,%d0
    move.l  #82,%d1             | 'R'
    trap    #15

.halt:
    stop    #0x2700
    bra.s   .halt
