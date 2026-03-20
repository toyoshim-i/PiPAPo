# CP/M Subsystem — Implementation Plan

Extracted from `docs/subsystems/cpm.md`. All phases complete.

---

### Phase 1 — Framework + Hello World ✅

**Status:** Complete. Committed as "Add CP/M subsystem Phase 1".

**Goal:** run a CP/M "Hello World" .COM that prints a string and
exits.

Steps:
1. ✅ Implement .COM loader (`cpm_loader.c`)
   - Detection by `.com` extension
   - Load binary at 0x0100
   - Zero page setup (JP stubs, BDOS/BIOS addresses)
   - Command-line tail at 0x0080, FCB parsing at 0x005C/0x006C
2. ✅ Implement minimal BDOS bridge (`cpm_bridge.c`)
   - Function 0: System Reset → exit
   - Function 2: Console Output
   - Function 9: Print String → write until `$`
3. ✅ Wire up ecpu-z80 trap handler for CALL 0x0005
4. ✅ **Test:** 11 tests (memory_map, cmdline, fcb_parse, hello_com, etc.)

**Files:**
- `src/kernel/subsys/cpm_bridge.h` — CP/M state structures
- `src/kernel/subsys/cpm_bridge.c` — BDOS/BIOS dispatch
- `src/kernel/subsys/cpm_loader.c` — .COM loader + memory setup

### Phase 2 — Console I/O ✅

**Status:** Complete. Committed as "Add CP/M console I/O (Phase 2)".

**Goal:** interactive console input/output.

Steps:
1. ✅ Function 1: Console Input (read + echo)
2. ✅ Function 3–5: Reader/Punch/List
3. ✅ Function 6: Direct Console I/O (non-blocking)
4. ✅ Function 7–8: Get/Set IOBYTE
5. ✅ Function 10: Read Console Buffer (line editing)
6. ✅ Function 11: Get Console Status
7. ✅ Function 12: Return Version Number (0x0022 = CP/M 2.2)
8. ✅ BIOS console functions (CONST, CONIN, CONOUT, LIST, PUNCH, READER)
9. ✅ **Test:** 13 tests (console_input, direct_io, echo_program, etc.)

### Phase 3 — FCB File Operations ✅

**Status:** Complete. Committed as "Add CP/M file operations (Phase 3)".

**Goal:** read and write files via FCB interface.

Steps:
1. ✅ FCB-to-path translation (`cpm_fcb_to_path()`)
2. ✅ Function 15: Open File (FCB → file descriptor)
3. ✅ Function 16: Close File
4. ✅ Function 20: Read Sequential (128-byte records to DMA)
5. ✅ Function 21: Write Sequential
6. ✅ Function 22: Make File (create)
7. ✅ Function 19: Delete File
8. ✅ Function 23: Rename File
9. ✅ Function 33–36, 40: Random read/write, compute file size, set random record
10. ✅ Function 13/14/24/25/26/29/32: Disk/DMA/user management
11. ✅ FCB position tracking (extent, current record)
12. ✅ Platform I/O abstraction (POSIX for host tests, syscalls for kernel)
13. ✅ **Test:** 9 tests (fcb_to_path, file_ops_real, random_write, etc.)

### Phase 4 — Directory Search ✅

**Status:** Complete. Committed as "Add CP/M directory search (Phase 4)".

**Goal:** directory listing via wildcard search.

Steps:
1. ✅ Function 17: Search First (directory listing)
2. ✅ Function 18: Search Next
3. ✅ DMA buffer directory entry format (32-byte FCB at slot 0)
4. ✅ Wildcard matching (`?` matches any character)
5. ✅ Platform directory abstraction (opendir/readdir for host, stubs for kernel)
6. ✅ **Test:** 4 tests (match_fcb, search_first_next, search_no_match, search_via_bdos)

### Phase 5 — Disk and User Management ✅

**Status:** Complete. Committed as "Add CP/M disk management (Phase 5)".

**Goal:** complete the remaining BDOS functions.

Steps:
1. ✅ Function 27: Get Alloc Vector (synthesized, all-allocated bitmap at 0xFD00)
2. ✅ Function 28: Write Protect Disk (no-op)
3. ✅ Function 29: Get Read-Only Vector
4. ✅ Function 30: Set File Attributes (no-op)
5. ✅ Function 31: Get Disk Parameter Block (IBM 3740 8" SSSD at 0xFCF0)
6. ✅ **Test:** 3 tests (alloc_vector, dpb, disk_noops)

### Phase 6 — Integration Testing ✅

**Status:** Complete. Committed as "Add CP/M integration tests (Phase 6)".

**Goal:** validate multi-BDOS sequences via Z80 execution.

1. ✅ version_and_disk_program: version + select disk + DPB access
2. ✅ multi_bdos_program: DMA + user codes + alloc vector
3. ✅ **Total test count:** 42 tests across all phases

### Phase 7 — Kernel Integration ✅

**Status:** Complete. Committed as "Wire CP/M subsystem into kernel".

**Goal:** connect the CP/M bridge to the kernel exec path and scheduler.

1. ✅ `.COM` loader (`exec_cpm.c`) — detects `.com` extension, loads binary
   at 0x0100, sets up CP/M memory map, FCB from argv, allocates Z80 state
2. ✅ `cpm_run_process()` — kernel-mode entry point; scheduler "returns" into
   the Z80 interpreter loop via `proc_setup_stack`
3. ✅ Subsystem registration: `SUBSYS_CPM = 2` in `proc.h`, `cpm_subsys_ops`
   in `subsys.c`, procfs entry "cpm"
4. ✅ Build system: `exec_cpm.c`, `cpm_bridge.c`, `ecpu_z80.c`, `ecpu_z80_alu.c`
   added to `KERNEL_SHARED_SOURCES` in `cmake/kernel.cmake`
5. ✅ `PPAP_KERNEL` compile definition added to `cmake/arm_m.cmake` and
   `cmake/m68k.cmake` to gate kernel-only code in shared sources

### Phase 8 — Userland Tests ✅

**Status:** Complete. 13 on-target tests using hand-assembled .COM binaries.

**Goal:** validate the full stack (exec → Z80 emulator → BDOS bridge → syscalls)
from user space.

1. ✅ 7 basic tests: exit, hello, charout, loop, version, halt, bad extension
2. ✅ 6 BDOS call tests: direct I/O (fn 6), select disk (fn 14/25), user code
   (fn 32), login vector (fn 24), file I/O (fn 15/16/20/21/22), warm boot
3. ✅ **Total test count:** 42 host tests + 13 userland tests = 55 tests

### Future — Real-World Testing

**Goal:** run popular CP/M applications.

| Application | Key BDOS Functions | Complexity |
|---|---|---|
| MBASIC | Console + random file I/O | Medium |
| Turbo Pascal | Console + sequential file I/O | Medium |
| WordStar | Console (ADM-3A) + file I/O | High (terminal) |
| dBASE II | Random file I/O + console | High |
| ZEXALL | CPU test (minimal BDOS) | Low (but exhaustive Z80 test) |
