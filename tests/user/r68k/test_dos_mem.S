| test_dos_mem.S — Human68k DOS memory management tests (R-format)
|
| Tests: _SETBLOCK, _MALLOC (query + alloc), _MFREE
| Exit code: 0 = all pass, N = number of failures.
|
| Registers on entry (set by R-format loader):
|   a0 = PMB base
|   a1 = end of memory block
|   a4 = image start (base + 0x100)

    .text
    .even
    .globl _start

_start:
    moveq   #0, %d7             | d7 = fail counter
    move.l  %a0, %a5            | save PMB base in a5

    | === _SETBLOCK: shrink process memory ===
    | Standard Human68k idiom: release unused pages.
    | block_addr = base + 0x10 (past 16-byte MMB header)
    | new_size = 0x1000 (4KB — plenty for this small program + stack)
    move.l  #0x1000, -(%sp)     | new_size
    move.l  %a5, %d0
    add.l   #0x10, %d0
    move.l  %d0, -(%sp)         | block_addr
    .word   0xFF4A              | _SETBLOCK
    addq.l  #8, %sp
    tst.l   %d0
    beq     1f
    addq.l  #1, %d7
1:

    | Update stack pointer to new block end (read from MMB header)
    move.l  8(%a5), %sp

    | === _MALLOC query: available memory ===
    | size = $FFFFFFFF → returns largest available block size
    move.l  #0xFFFFFFFF, -(%sp)
    .word   0xFF48              | _MALLOC
    addq.l  #4, %sp
    | d0 should be positive (available bytes)
    tst.l   %d0
    bgt     1f
    addq.l  #1, %d7
1:

    | === _MALLOC: allocate 256 bytes ===
    move.l  #0x100, -(%sp)
    .word   0xFF48              | _MALLOC
    addq.l  #4, %sp
    move.l  %d0, %d5            | save pointer in d5
    | Check for error: high byte $81 or $82 indicates failure
    move.l  %d5, %d0
    swap    %d0
    lsr.w   #8, %d0
    cmp.b   #0x81, %d0
    beq     .Lmalloc_fail
    cmp.b   #0x82, %d0
    beq     .Lmalloc_fail

    | Write and read back: verify allocated memory is usable
    move.l  %d5, %a2
    move.l  #0xDEADBEEF, (%a2)
    cmp.l   #0xDEADBEEF, (%a2)
    beq     1f
.Lmalloc_fail:
    addq.l  #1, %d7
1:

    | === _MFREE: free the allocated block ===
    move.l  %d5, -(%sp)
    .word   0xFF49              | _MFREE
    addq.l  #4, %sp
    tst.l   %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === _MFREE with bad address: should fail ===
    move.l  #0x12345678, -(%sp)
    .word   0xFF49              | _MFREE
    addq.l  #4, %sp
    | Should return negative error (-7 = invalid memory block)
    tst.l   %d0
    bmi     1f
    addq.l  #1, %d7
1:

    | === Exit with fail count ===
    move.w  %d7, -(%sp)
    .word   0xFF4C              | _EXIT2
