# MS-DOS Subsystem Proposal

An MS-DOS personality layer for PPAP, following the same pattern as the
Human68k subsystem (`human68k_bridge.c`) and CP/M subsystem
(`cpm_bridge.c`).  Translates DOS INT 21h calls and related interrupts
into PPAP syscalls, allowing DOS .COM and .EXE (MZ) binaries to run on
any PPAP host architecture through the eCPU i8086 emulator or natively
on a V30/8086 target.

## Current status (2026-04-19)

- Native ia16 path is the only one wired today: pcxt boots, INT 21h is
  trapped through `dos_trap.S`, and `.COM` and `.EXE` programs run on
  real-mode 8086 directly.  The i8086 eCPU path described below is not
  built yet.
- Phases **D-1, D-2, D-3, D-4 (a/b/c), and D-5a.1** are complete.
  `test_msdos` reports 53/53 sub-tests on pcxt covering exit, console
  I/O, version, OPEN / CREATE / CLOSE, READ / WRITE, DELETE / LSEEK,
  MKDIR / RMDIR, DUP, RENAME, plus error-path coverage (bad handle,
  bad whence, invalid drive, missing file, double close), MZ .EXE
  loading (header parse, zero-reloc, single-reloc JMP FAR, multi-
  segment DS reload), and the MCB-shifted run layout.
- INT 21h surface today: AH=01h, 02h, 06h, 08h, 09h, 0Ah, 0Bh (stub),
  19h, 2Ah/2Ch (stubs — no RTC), 30h, 39h, 3Ah, 3Bh, 3Ch, 3Dh, 3Eh,
  3Fh, 40h, 41h, 42h, 45h, 46h, 47h, 4Ch, 56h.  Memory functions
  (AH=48h/49h/4Ah) are still TBD (D-5a.2/3); the static MCB header is
  in place so crt0s that walk the chain at entry find a valid block.
- `SUBSYS_MSDOS = 4` is registered; `subsys.c` wires `msdos_subsys_ops`.
- Implementation files landed under
  `src/kernel/core/subsys/msdos/`: `dos_bridge.{c,h}`, `dos_host.{c,h}`
  (MCB + PSP build, image load, initial frame), `com_loader.{c,h}` and
  `exe_loader.{c,h}` (loader_t registration), and the trap entry
  `src/arch/i16/kernel/core/dos_trap.S`.
- All path-taking syscalls (`sys_open`, `sys_unlink`, `sys_mkdir`,
  `sys_rename`, `sys_chdir`, `sys_stat`, …) take `(page_id_t, uint16_t)`
  for each path argument.  The DOS bridge stages resolved paths into
  a kmem-backed scratch slot inside `dos_data_page` and passes the
  `(page, off)` pair straight to `sys_*` — no PSP staging, no
  per-arch ifdef.

---

## 1. Goals and Scope

### 1.1 Primary Goal

Run MS-DOS 2.x–3.x compatible .COM and .EXE programs on PPAP:

- Load DOS .COM flat binaries (single segment, max 64 KB).
- Load DOS .EXE (MZ format) multi-segment binaries with relocation.
- Translate INT 21h DOS API calls to PPAP VFS, process, and TTY operations.
- Translate INT 10h video calls to PPAP TTY output (text mode only).
- Provide a PSP (Program Segment Prefix) compatible with DOS 2.x+.
- Support drive letter mapping (A:, B:, C:) to PPAP mount points.
- Integrate with the ptrace/trace subsystem (new ABI tag for DOS calls).

### 1.2 Extended Goals

- FCB-based file operations (INT 21h AH=0Fh–24h) for DOS 1.x compat.
- INT 10h direct video buffer access (CGA-compatible 80×25 text).
- INT 16h keyboard input with scan codes.
-AZOMBIE TSR (Terminate and Stay Resident) support.
- INT 2Fh multiplex interrupt (limited, for PRINT and ASSIGN).
- Batch file execution (COMMAND.COM-style `.BAT` interpreter).
- Environment variable expansion.

### 1.3 Out of Scope

- EMS/XMS extended memory.
- DOS 4.x+ features (large partition support, INT 25h/26h absolute disk).
- DPMI (DOS Protected Mode Interface) or DOS extenders.
- Graphics modes (INT 10h AH=00h mode changes, direct VRAM pixel access).
- Sound (INT 61h PC speaker, Sound Blaster).
- Networking (INT 60h–6Fh redirector interface).
- Full COMMAND.COM shell (PPAP's own shell is used for launching DOS programs).

---

## 2. Architecture

### 2.1 Subsystem Registration

Following the existing pattern in `subsys.h`:

```c
/* src/kernel/proc/proc.h */
#define SUBSYS_PPAP      0
#define SUBSYS_HUMAN68K  1
#define SUBSYS_CPM       2
#define SUBSYS_MSDOS     3    /* new */

/* SUBSYS_MAX must be >= 4 (already 4 in current code) */
```

The DOS subsystem registers its `subsys_ops_t` in `subsys_ops_table[3]`:

```c
const subsys_ops_t msdos_subsys_ops = {
    .on_crash  = msdos_on_crash,
    .on_signal = msdos_on_signal,
    .on_init   = msdos_on_init,
};
```

### 2.2 Execution Model

DOS programs run on the i8086 eCPU emulator (see `docs/proposals/i8086_ecpu.md`)
or natively on a V30/8086 target.  The execution flow:

```
User: /bin/sh
  │
  │ execve("/subsys/dos/hello.com")
  ▼
Kernel: exec detects .COM or MZ signature
  │
  │ subsys = SUBSYS_MSDOS
  ▼
dos_com_loader.c
  │ Allocates segment(s), builds PSP, loads binary
  │ Sets up i8086 eCPU state (or V30 segments for native)
  ▼
dos_run_process()
  │
  │ ecpu_i8086_ops.run(cpu)  ──→  emulator loop
  │                                    │
  │                                    │ INT 21h → ECPU_TRAP_SWI
  │                                    ▼
  │                              dos_bridge.c
  │                                    │ Translates to PPAP syscalls
  │                                    │ Returns result in AX
  │                                    ▼
  │                              emulator continues
  │
  │ INT 21h AH=4Ch (EXIT) → ECPU_TRAP_EXIT
  ▼
Kernel: process exits, parent wakes up
```

On a native V30/8086 target, the i8086 eCPU is not needed — the DOS program
runs directly on the CPU.  INT 21h is hooked to the kernel trap handler,
which calls the same `dos_bridge.c` functions.

### 2.3 Relationship to Existing Subsystems

| Aspect | CP/M | Human68k | MS-DOS |
|--------|------|----------|--------|
| Host CPU | Z80 (eCPU) | m68k (native or eCPU) | i8086 (eCPU or native V30) |
| API entry | CALL 0x0005 | F-line ($FFxx) | INT 21h |
| Binary format | .COM (flat @ 0x0100) | .x/.r (relocatable) | .COM (flat @ 0x0100) + .EXE (MZ) |
| Memory model | Flat 64 KB | Flat 24-bit | Segmented 20-bit (1 MB) |
| Bridge file | `cpm_bridge.c` | `human68k_bridge.c` | `dos_bridge.c` |
| Loader file | `cpm_host.c` | `human68k_host.c` | `dos_com_loader.c` |
| Per-process state | `cpm_state_t` | `h68k_proc_t` | `dos_proc_t` |
| Drive mapping | A: → `/a/` | A: → `/a/` | A: → `/a/` |

---

## 3. DOS Memory Model (Emulated)

The long-term spec below assumes a 1 MB flat memory provided by the i8086
eCPU.  The current native ia16 implementation is more constrained and is
documented first.

### 3.0 Current native ia16 layout

Each DOS process owns one **contiguous run** of physical pages, picked at
load time by `mem_region_page_alloc_largest_contiguous(DOS_SEG_PAGES,
DOS_SEG_PAGES_MAX, …)`.  `DOS_SEG_PAGES = 17` (68 KB) is the floor —
enough to hold a 16-byte MCB header plus a full 64 KB `proc_seg` window.
`DOS_SEG_PAGES_MAX = 32` (128 KB) is the ceiling, sized to fit common
DOS apps (e.g. zork1) while leaving headroom on the limited pcxt page
pool (~120 pages total).

Layout inside the run:

```
base_id:0000   ┌──────────────────────────────┐
               │  MCB 'Z' (16 B = 1 paragraph)│  proc_seg - 1
               │   sig=0x5A owner=proc_seg    │
               │   size=run_paragraphs - 1    │
proc_seg:0000  ├──────────────────────────────┤
               │  PSP (256 B)                 │  proc_seg
               │   00: INT 20h    02: mem_top │
               │   50: INT 21h+RETF           │
               │   80: command tail           │
proc_seg:0100  ├──────────────────────────────┤
               │  .COM image  -or-            │  load_seg = proc_seg + 0x10
               │  .EXE image (after MZ hdr)   │  for .EXE
               │                              │
(SS:SP region) ├──────────────────────────────┤
               │  Initial HW + SW frame       │
               │   (matches trap.S restore)   │
               │  Stack grows down            │
end-of-run     └──────────────────────────────┘
```

Key invariants:

- `base_linear = mem_region_page_linear(base_id)` is the start of the run.
- `proc_seg = (base_linear >> 4) + 1` — the MCB occupies paragraph 0, so
  PSP is one paragraph in.
- For `.COM`: `CS = DS = ES = SS = proc_seg`, `IP = 0x100`, `SP = 0xFFFE`.
  The full 64 KB `proc_seg:0000..proc_seg:FFFF` window is backed (this is
  why the floor is 17 pages: 16 B MCB + 65 536 B window = 65 552 B, which
  fits in 17 × 4 096 = 69 632 B).
- For `.EXE`: `load_seg = proc_seg + 0x10`, `CS = init_cs + load_seg`,
  `IP = init_ip`, `SS = init_ss + load_seg`, `SP = init_sp`,
  `DS = ES = proc_seg`.  Multi-segment access is fine — the run extends
  beyond the 64 KB `proc_seg` window up to 128 KB and any `(seg, off)`
  inside it is reachable through the bridge's `dos_to_linear` /
  `cpu_ops->read8/write8` path.
- The whole run is owned via `image.data` with `PROC_IMAGE_SEG_OWNED`, so
  it's freed in one shot by `image_release_owned_segments()` on exit.

The bridge's INT 21h handlers that take a `DS:DX` user buffer (READ,
WRITE, AH=09h string print, etc.) compute `flat = (DS << 4) + DX` and
range-check against `[base_linear, base_linear + image.data.size)`, so
DS can point anywhere inside the run — including the MCB paragraph
itself, which is intentional so a crt0 can read the MCB at `DS-1:0`.

#### MCB emulation status

At load time, `dos_host.c` writes a single 'Z' MCB at paragraph 0 of the
run via `dos_write_mcb(base_id, 0, 'Z', proc_seg, payload_para)`, where
`payload_para = run_paragraphs - 1`.  `PSP[0x02] mem_top` is set
consistently to `proc_seg + payload_para` so `(mem_top - proc_seg) ==
size_in_MCB`, matching the contract a crt0 expects when it cross-checks
those two fields.

Runtime memory operations are not yet wired:

- **AH=4Ah Resize** — pending (D-5a.2).  Will validate the MCB at `ES-1`,
  split into `'M'(size=BX)` + `'Z'(owner=0, size=remainder)`.
- **AH=48h Allocate / AH=49h Free** — pending (D-5a.3).  Will walk the
  in-run MCB chain, allocating from the first free block that fits and
  coalescing on free.

Until those land, a crt0 that calls AH=48h/49h/4Ah gets DOS error 1
("invalid function") — it should fall through gracefully, but some C
runtimes treat this as fatal and exit silently.  Programs that walk the
MCB chain at entry already work because the static header is correct.

### 3.1 Memory Map (i8086 eCPU, long-term)

The remainder of section 3 describes the long-term design for the eCPU
path — a 1 MB simulated address space with a real DOS-style MCB chain.
This is not built today; the native ia16 path in §3.0 is what runs.

```
i8086 eCPU address space (1 MB)
┌──────────────────────────────────┐ 0xFFFFF
│  Reserved (BIOS data area sim)   │ 0xF0000
├──────────────────────────────────┤
│  Unused / available              │
│                                  │
├──────────────────────────────────┤
│  Program segment(s)             │
│  ┌───────────────────────┐      │
│  │ PSP (256 bytes)       │ seg:0000
│  │ Program code/data     │ seg:0100
│  │ Stack (grows down)    │ seg:FFFF ↓
│  └───────────────────────┘      │
├──────────────────────────────────┤
│  DOS data area (simulated)      │
│  Environment segment             │ 0x00800
├──────────────────────────────────┤
│  Interrupt vector table (IVT)    │ 0x00000  (256 × 4B = 1 KB)
│  BIOS data area (simulated)     │ 0x00400
└──────────────────────────────────┘
```

### 3.2 Segment Allocation

The DOS bridge maintains a simple free-list of paragraph-aligned blocks:

```c
#define DOS_PARA_SIZE       16      /* bytes per paragraph */
#define DOS_MCB_SIGNATURE_M 0x4D    /* 'M' — more blocks follow */
#define DOS_MCB_SIGNATURE_Z 0x5A    /* 'Z' — last block */

typedef struct dos_mcb {
    uint8_t  signature;     /* 'M' or 'Z' */
    uint16_t owner_psp;     /* PSP segment of owner (0 = free) */
    uint16_t size;          /* size in paragraphs (excluding header) */
} dos_mcb_t;
```

INT 21h memory functions map to MCB operations:

| AH | Function | Operation |
|----|----------|-----------|
| 48h | Allocate memory | Find free MCB, split, return segment |
| 49h | Free memory | Mark MCB as free, merge adjacent |
| 4Ah | Resize memory | Grow or shrink the MCB block |

Internally, MCB operations translate to PPAP page allocations as backing
storage.

### 3.3 .COM Loading

```c
int dos_load_com(i8086_state_t *cpu, dos_proc_t *dos,
                 const char *path)
{
    /* 1. Allocate one 64 KB segment */
    uint16_t seg = dos_alloc_segment(dos, 0x1000);  /* 64 KB = 0x1000 paragraphs */

    /* 2. Build PSP at seg:0000 */
    dos_build_psp(cpu, seg, dos);

    /* 3. Load binary at seg:0100 */
    int fd = sys_open(path, O_RDONLY, 0);
    int size = sys_read(fd, i8086_translate(cpu, seg, 0x0100), 0xFEFF);
    sys_close(fd);

    /* 4. Set registers */
    cpu->cs = seg;
    cpu->ds = seg;
    cpu->es = seg;
    cpu->ss = seg;
    cpu->sp = 0xFFFE;
    cpu->ip = 0x0100;

    return 0;
}
```

### 3.4 .EXE (MZ) Loading

```c
int dos_load_exe(i8086_state_t *cpu, dos_proc_t *dos,
                 const char *path)
{
    struct mz_header hdr;
    /* 1. Read MZ header */
    /* 2. Calculate load size from header */
    uint32_t code_size = (hdr.page_count * 512)
                       - (hdr.header_size * 16)
                       - (hdr.last_page_size ? 512 - hdr.last_page_size : 0);
    uint32_t total_para = (code_size + 15) / 16 + hdr.min_alloc;

    /* 3. Allocate segments: PSP (0x10 para) + program */
    uint16_t psp_seg = dos_alloc_segment(dos, 0x10 + total_para);
    uint16_t load_seg = psp_seg + 0x10;

    /* 4. Build PSP */
    dos_build_psp(cpu, psp_seg, dos);

    /* 5. Load program image after MZ header */
    sys_lseek(fd, hdr.header_size * 16, SEEK_SET);
    sys_read(fd, i8086_translate(cpu, load_seg, 0), code_size);

    /* 6. Apply segment relocations */
    for (int i = 0; i < hdr.reloc_count; i++) {
        uint16_t off, seg;
        /* Read relocation entry */
        /* Add load_seg to the word at seg:off in the loaded image */
        uint32_t addr = ((uint32_t)(seg + load_seg) << 4) + off;
        uint16_t val = i8086_read16(cpu, addr);
        i8086_write16(cpu, addr, val + load_seg);
    }

    /* 7. Set registers from header */
    cpu->cs = hdr.init_cs + load_seg;
    cpu->ip = hdr.init_ip;
    cpu->ss = hdr.init_ss + load_seg;
    cpu->sp = hdr.init_sp;
    cpu->ds = psp_seg;
    cpu->es = psp_seg;

    return 0;
}
```

---

## 4. INT 21h Bridge

### 4.1 Dispatch

The eCPU traps INT 21h as `ECPU_TRAP_SWI` with `param = 0x21`.  The trap
handler dispatches based on AH:

```c
int dos_trap_handler(ecpu_state_t *cpu, int trap_type,
                     uint32_t param, void *ctx)
{
    dos_proc_t *dos = (dos_proc_t *)ctx;
    i8086_state_t *i86 = (i8086_state_t *)cpu;

    if (trap_type == ECPU_TRAP_SWI) {
        switch (param) {
        case 0x20: return dos_exit_int20(i86, dos);
        case 0x21: return dos_int21h(i86, dos);
        case 0x10: return dos_int10h(i86, dos);  /* Video */
        case 0x16: return dos_int16h(i86, dos);  /* Keyboard */
        default:   return ECPU_TRAP_HANDLED;     /* Ignore unknown */
        }
    }
    if (trap_type == ECPU_TRAP_HALT)
        return ECPU_TRAP_EXIT;

    return ECPU_TRAP_UNHANDLED;
}
```

### 4.2 Function Coverage

The INT 21h bridge is implemented in phases, starting with the most
commonly used functions.

#### Phase 1 — Core Functions (~20 functions)

These cover enough to run simple DOS .COM utilities:

| AH | Name | PPAP mapping | Status |
|----|------|-------------|--------|
| 01h | Read char with echo | `sys_read(0)` + `sys_write(1)` | done |
| 02h | Write character | `sys_write(1)` | done |
| 06h | Direct console I/O | `sys_read(0)` / `sys_write(1)` (non-blocking) | TODO |
| 08h | Read char no echo | `sys_read(0)` | done |
| 09h | Print string ($-term) | `sys_write(1)` until '$' | done |
| 0Ah | Buffered input | Line-edit read into DS:DX buffer | done |
| 0Bh | Check input status | `sys_poll(0)` or `sys_ioctl(FIONREAD)` | stub |
| 19h | Get current drive | Return `dos->current_drive` | done |
| 25h | Set interrupt vector | Store in emulated IVT | TODO |
| 2Ah | Get date | `sys_gettimeofday()` → year/month/day | stub (fixed date) |
| 2Ch | Get time | `sys_gettimeofday()` → hour/min/sec | stub (fixed time) |
| 30h | Get DOS version | Return 3.30 (static) | done |
| 35h | Get interrupt vector | Read from emulated IVT | TODO |
| 3Ch | Create file | `sys_open(path, O_CREAT\|O_TRUNC\|O_WRONLY)` | done |
| 3Dh | Open file | `sys_open(path, mode)` | done |
| 3Eh | Close file | `sys_close(handle)` | done |
| 3Fh | Read file | `sys_read(handle, buf, count)` | done |
| 40h | Write file | `sys_write(handle, buf, count)` | done |
| 41h | Delete file | `sys_unlink(path)` | done |
| 42h | Seek (LSEEK) | `sys_lseek(handle, offset, whence)` | done |
| 43h | Get/set file attributes | Stub (return 0x20 archive) | TODO |
| 47h | Get current directory | Return `dos->cwd[drive]` | done |
| 4Ch | Terminate with code | `sys_exit(code)` | done |

#### Phase 2 — File System Extensions (~15 functions)

| AH | Name | PPAP mapping |
|----|------|-------------|
| 39h | Create directory | `sys_mkdir(path)` |
| 3Ah | Remove directory | `sys_rmdir(path)` |
| 3Bh | Change directory | Update `dos->cwd[drive]` |
| 44h | IOCTL | Subset: AX=4400h (get device info) |
| 45h | Duplicate handle | `sys_dup(handle)` |
| 46h | Force duplicate | `sys_dup2(old, new)` |
| 48h | Allocate memory | MCB allocation |
| 49h | Free memory | MCB free |
| 4Ah | Resize memory | MCB resize |
| 4Bh | EXEC | `sys_vfork()` + `sys_execve()` |
| 4Dh | Get child return code | `sys_waitpid()` |
| 56h | Rename | `sys_rename(old, new)` |
| 57h | Get/set file date | Stub |
| 5Ah | Create temporary file | `sys_open()` with generated name |
| 5Bh | Create new file | `sys_open(O_CREAT\|O_EXCL)` |

#### Phase 3 — FCB Operations (~10 functions)

For DOS 1.x compatibility.  FCB (File Control Block) operations translate
to PPAP handle-based file I/O internally:

| AH | Name |
|----|------|
| 0Fh | Open file (FCB) |
| 10h | Close file (FCB) |
| 11h | Search first (FCB) |
| 12h | Search next (FCB) |
| 14h | Sequential read (FCB) |
| 15h | Sequential write (FCB) |
| 16h | Create file (FCB) |
| 21h | Random read (FCB) |
| 22h | Random write (FCB) |
| 27h | Random block read (FCB) |
| 28h | Random block write (FCB) |

FCB-to-handle translation follows the same pattern as CP/M's FCB table
in `cpm_bridge.c`.

### 4.3 Handle Translation

DOS uses handle numbers 0–19 (JFT, Job File Table, 20 entries in the PSP).
The bridge maps these to PPAP file descriptors:

```c
#define DOS_MAX_HANDLES  20

typedef struct dos_proc {
    /* Handle table: dos_handle → ppap_fd (-1 = closed) */
    int handle_to_fd[DOS_MAX_HANDLES];

    /* Standard handles pre-opened at load time:
     *   0 = stdin  → PPAP fd 0
     *   1 = stdout → PPAP fd 1
     *   2 = stderr → PPAP fd 2
     *   3 = stdaux → PPAP fd 2 (serial, mapped to stderr)
     *   4 = stdprn → PPAP fd 2 (printer, mapped to stderr)
     */

    uint8_t  current_drive;          /* 0=A, 1=B, ... */
    char     cwd[26][DOS_PATH_MAX];  /* per-drive CWD */

    /* Environment block */
    uint16_t env_seg;                /* segment of environment strings */

    /* PSP segment of this process */
    uint16_t psp_seg;

    /* MCB chain head (for memory management) */
    uint16_t mcb_head_seg;

    /* FCB-to-handle mapping (for FCB operations) */
    struct {
        uint16_t fcb_seg;    /* segment:offset of FCB (0 = free) */
        uint16_t fcb_off;
        int      fd;         /* PPAP fd */
        uint32_t file_pos;   /* current position */
    } fcb_table[8];

    /* Directory search state (for FindFirst/FindNext) */
    char     search_pattern[DOS_PATH_MAX];
    void    *search_dir;     /* opaque DIR handle */
    uint8_t  search_attr;    /* attribute filter */
} dos_proc_t;
```

### 4.4 Path Translation

DOS paths use backslash separators and drive letters.  The bridge
converts them to PPAP paths.

**Drive assignment.**  Rather than the generic `X: → /x/` scheme, PPAP
uses a small fixed table so DOS programs land in meaningful PPAP
directories without requiring target-specific mount config:

| Drive | Mapping | Per-process? | Notes |
|-------|---------|--------------|-------|
| `C:`  | executable directory of the running .COM/.EXE | yes | set at exec time; mirrors how CP/M used `A:` |
| `Z:`  | `/` (PPAP root) | no | stable escape hatch to the full FS |
| others | unassigned | — | access returns DOS error 15 (invalid drive) |

The default drive is `C:`.  `dos_proc_t` gains `exec_dir[]` (captured
by the loader from `dirname(path)`) so `C:` can be resolved without a
global table.

**Resolution examples:**

```
C:\FOO\BAR.TXT       →  {exec_dir}/FOO/BAR.TXT
C:BAR.TXT            →  {exec_dir}/{cwd[C]}/BAR.TXT
Z:\etc\hostname      →  /etc/hostname
Z:etc                →  /{cwd[Z]}/etc
BAR.TXT              →  {current_drive_root}/{cwd[current]}/BAR.TXT
\tmp\file            →  {current_drive_root}/tmp/file
```

**Rules:**

1. Backslashes become forward slashes.
2. Absolute (leading `\` or `/`) vs relative is resolved against the
   per-drive CWD.
3. **Case-sensitive lookup for now.**  See §12.1 — case-folding is
   deferred.  DOS programs that emit lowercase names will see PPAP
   files as-is; programs that uppercase will need matching uppercase
   files in the image.
4. Unassigned drives return error 15 without touching the VFS.

```c
int dos_resolve_path(dos_proc_t *dos, const char *dos_path,
                     char *ppap_path, int ppap_path_size);
```

### 4.5 Error Mapping

DOS returns errors via the carry flag (CF) and AX.  The bridge maps
PPAP errno values to DOS error codes:

| PPAP errno | DOS error (AX) | DOS meaning |
|-----------|---------------|------------|
| ENOENT | 2 | File not found |
| ENOTDIR | 3 | Path not found |
| EMFILE | 4 | Too many open files |
| EACCES | 5 | Access denied |
| EBADF | 6 | Invalid handle |
| ENOMEM | 8 | Insufficient memory |
| EEXIST | 80 | File exists |

On error, the bridge sets CF=1 and AX=error code.  On success, CF=0.

---

## 5. PSP (Program Segment Prefix)

The bridge constructs a 256-byte PSP at the base of each program's segment:

```c
void dos_build_psp(i8086_state_t *cpu, uint16_t psp_seg,
                   dos_proc_t *dos)
{
    uint32_t base = (uint32_t)psp_seg << 4;

    /* 00h: INT 20h (CD 20) — terminate program */
    i8086_write16(cpu, base + 0x00, 0x20CD);

    /* 02h: Memory size (top of allocated segment) */
    i8086_write16(cpu, base + 0x02, psp_seg + /* allocated paragraphs */);

    /* 05h: Far call to DOS dispatcher (INT 21h + RETF) */
    i8086_write8(cpu, base + 0x05, 0xEA);    /* JMP FAR */
    /* ... address of INT 21h handler ... */

    /* 0Ah–15h: Saved interrupt vectors (INT 22h, 23h, 24h) */
    /* Read current values from IVT and save */

    /* 2Ch: Environment segment */
    i8086_write16(cpu, base + 0x2C, dos->env_seg);

    /* 50h: INT 21h + RETF (alternate DOS entry) */
    i8086_write8(cpu, base + 0x50, 0xCD);
    i8086_write8(cpu, base + 0x51, 0x21);
    i8086_write8(cpu, base + 0x52, 0xCB);     /* RETF */

    /* 5Ch: FCB 1 (parsed from command line) */
    /* 6Ch: FCB 2 (parsed from command line) */

    /* 80h: Command tail */
    /* Copy from argv, DOS format: length byte + space-separated args + CR */
}
```

---

## 6. Video and Keyboard (INT 10h / INT 16h)

### 6.1 INT 10h — Video Services

Only text-mode services are supported.  All output maps to PPAP TTY:

| AH | Function | Implementation |
|----|----------|---------------|
| 00h | Set video mode | Stub (always report mode 3 = 80×25 color) |
| 02h | Set cursor position | ANSI escape: `\033[row;colH` |
| 03h | Get cursor position | Return stored cursor pos |
| 06h | Scroll up | ANSI escape: `\033[nS` |
| 07h | Scroll down | ANSI escape: `\033[nT` |
| 09h | Write char+attr | `sys_write(1)` (ignore attribute) |
| 0Eh | Teletype output | `sys_write(1)` (simplest path) |
| 0Fh | Get video mode | Return mode 3, 80 columns, page 0 |

### 6.2 INT 16h — Keyboard Services

| AH | Function | Implementation |
|----|----------|---------------|
| 00h | Read key | `sys_read(0)` → AH=scan code, AL=ASCII |
| 01h | Check key | `sys_poll(0)` → ZF=0 if key available |
| 02h | Get shift flags | Return 0 (no shift key tracking) |

Scan code generation is approximate — the bridge maps ASCII codes to
PC-compatible scan codes for common keys.

---

## 7. Trace Integration

### 7.1 ABI Tag

```c
#define PPAP_TRACE_ABI_DOS_INT21  5   /* in ptrace.h */
```

### 7.2 Event Generation

The bridge generates trace events at INT 21h entry and exit:

```c
/* At INT 21h entry */
trace_subsys_event(PPAP_TRACE_EVENT_SUBSYS_ENTER,
                   PPAP_TRACE_ABI_DOS_INT21,
                   regs->ah,           /* function number */
                   args);              /* AX, BX, CX, DX, DS, SI */

/* At INT 21h exit */
trace_subsys_event(PPAP_TRACE_EVENT_SUBSYS_EXIT,
                   PPAP_TRACE_ABI_DOS_INT21,
                   regs->ah,
                   &ret);              /* AX return value */
```

### 7.3 Trace Output

```
$ trace --subsys hello.com
[subsys:dos] INT21 AH=09 WRITE_STRING DS:DX=0x0180
[subsys:dos] INT21 AH=3D OPEN DS:DX=0x0150 "DATA.TXT" AL=00 → handle=5
[subsys:dos] INT21 AH=3F READ BX=5 CX=128 DS:DX=0x0200 → 128
[subsys:dos] INT21 AH=3E CLOSE BX=5 → OK
[subsys:dos] INT21 AH=4C EXIT AL=00
```

### 7.4 pdb Debugger

The pdb debugger gains DOS awareness through the existing multi-ABI
infrastructure:

- `show abi` reports `dos/int21`
- Register display uses 8086 register set (AX, BX, CX, DX, SI, DI, BP,
  SP, CS, DS, ES, SS, IP, FLAGS)
- `disas` uses 8086 disassembly
- Breakpoints work through the eCPU step/trap mechanism

---

## 8. New Files

Files actually landed:

```
src/kernel/core/subsys/msdos/
  dos_bridge.c        — INT 21h dispatch and function implementations
  dos_bridge.h        — dos_proc_t, constants, public API
  dos_host.c          — PSP build, .COM image load, initial user frame
  dos_host.h          — dos_build_com_image() interface
  com_loader.c        — .COM detection and loader_t registration
  com_loader.h        — Loader API

src/arch/i16/kernel/core/
  dos_trap.S          — Native i16 INT 21h ISR (real-mode 8086)
```

Changes to existing files:

| File | Change |
|------|--------|
| `src/kernel/common/core/subsys_info.h` | Add `SUBSYS_MSDOS = 4` (slot 3 taken by SOS) |
| `src/kernel/core/subsys/subsys.c` | Register `msdos_subsys_ops` |
| `src/kernel/core/exec/loader.c` | Include `com_loader`, add to registry |
| `cmake/kernel.cmake` | Add `KERNEL_SUBSYS_MSDOS_SOURCES` |
| `src/target/pcxt/CMakeLists.txt` | Link MSDOS sources, define `PPAP_ENABLE_MSDOS=1` |
| `src/target/pcxt/kernel/core/driver/timer_pit.c` | Install INT 21h vector → `i16_dos_isr` |

---

## 9. Build Configuration

```cmake
option(PPAP_ENABLE_MSDOS "Enable MS-DOS subsystem" OFF)

if(PPAP_ENABLE_MSDOS)
    target_sources(ppap PRIVATE
        src/kernel/core/subsys/msdos/dos_bridge.c
        src/kernel/core/subsys/msdos/dos_host.c
        src/kernel/core/subsys/msdos/com_loader.c
    )
    target_compile_definitions(ppap PRIVATE PPAP_ENABLE_MSDOS=1)
endif()
```

The DOS subsystem requires the i8086 eCPU (`PPAP_ENABLE_ECPU_I8086`)
unless building for a native V30/8086 target.

---

## 10. Implementation Phases

### Phase D-1: .COM Loader and Minimal INT 21h — DONE (native ia16)

**Goal**: "Hello, world" DOS .COM program runs and prints output.

1. ✓ `com_loader.c` + `dos_host.c` — .COM detection, segment alloc,
   PSP build, image stream, initial user frame.
2. ✓ `dos_bridge.c` — INT 21h dispatch covering AH=01h, 02h, 08h, 09h,
   0Ah, 0Bh (stub), 19h, 2Ah/2Ch (stub), 30h, 4Ch.
3. ✓ PSP with INT 20h, memory top, INT 21h+RETF entry, command tail.
4. ✓ Hand-assembled .COM tests run: `tests/user/test_msdos.c` passes
   12/12 sub-tests on pcxt (exit_zero, exit_code, charout, hello,
   version).

**Verification done**: pcxt runs `test_msdos` end-to-end through
vfork+execve+pipe capture, with serial output matching expectations.

### Phase D-2: File I/O

**Goal**: DOS programs can create, open, read, write, seek, close, and
delete files via INT 21h AH=3Ch–42h on PPAP-backed storage, with the
case-sensitive drive scheme from §4.4.

The phase is broken into four self-contained steps; each lands as its
own commit with its own test additions in `test_msdos.c`.

D-2a–c were originally split further, but path infra (a), handle
table + CLOSE (b), and OPEN/CREATE (c) have no testable surface in
isolation — the first user-visible INT 21h round-trip needs all three.
They're combined into **D-2abc** below.

#### D-2abc: Path infra + handle table + OPEN/CREATE/CLOSE

**State changes in `dos_proc_t`:**

```c
char    exec_dir[DOS_PATH_MAX];   /* dirname() of the .COM/.EXE path */
char    cwd_c[DOS_PATH_MAX];      /* CWD on drive C:, default ""    */
char    cwd_z[DOS_PATH_MAX];      /* CWD on drive Z:, default ""    */
uint8_t current_drive;            /* 'C'-'A' = 2 by default          */
```

`com_loader` populates `exec_dir` from `argv[0]` (which `exec.c`
defaults to the `execve` path).  `msdos_on_init` zeros the CWDs and
sets `current_drive = 2` (C:).

**Path resolver:**

```c
int dos_resolve_path(dos_proc_t *dos, const char *dos_path,
                     char *out, int out_size);
```

Implements the rules in §4.4 with case-sensitive matching.  Returns
0 on success, `-DOS_ERR_INVALID_DRIVE` (15) for unassigned drives,
`-DOS_ERR_PATH_NOT_FOUND` (3) for output buffer overflow.

**Handle table:**

```c
static int dos_alloc_handle(dos_proc_t *dos, int fd) {
  for (int h = 5; h < DOS_MAX_HANDLES; h++)
    if (dos->handle_to_fd[h] < 0) { dos->handle_to_fd[h] = fd; return h; }
  return -DOS_ERR_TOO_MANY_OPEN;  /* 4 */
}
static int dos_lookup_fd(dos_proc_t *dos, int handle) {
  if (handle < 0 || handle >= DOS_MAX_HANDLES) return -DOS_ERR_INVALID_HANDLE;
  int fd = dos->handle_to_fd[handle];
  return fd < 0 ? -DOS_ERR_INVALID_HANDLE : fd;
}
```

Handles 0–4 (stdin/stdout/stderr/aux/prn) are pre-opened in
`msdos_on_init` and remain reserved for the life of the process.

**AH=3Ch CREATE:**
- DS:DX = path; CX = attribute (low byte: bit 0 RO, bit 1 hidden, bit 2 system, bit 5 archive — only RO honored for now via `mode 0444`, others ignored).
- Maps to `sys_open(path, O_CREAT | O_TRUNC | O_WRONLY, mode)`.

**AH=3Dh OPEN:**
- DS:DX = path; AL access mode:
  - low 3 bits: 0=O_RDONLY, 1=O_WRONLY, 2=O_RDWR, others → error 12 (invalid access).
  - bits 4–6: sharing — ignored on PPAP.
  - bit 7: inherit — ignored.
- Maps to `sys_open(path, flags, 0)`.

Both copy the DOS path out of the user segment (small kernel-stack
buffer via `cpu_ops->read8` over `dos_to_linear(ds, dx)`), call
`dos_resolve_path`, then `sys_open`.  On success allocate a handle and
return it in AX.  On any error, set CF and AX from §4.5.

**AH=3Eh CLOSE:**
- handle 0–4: refuse (DOS allows but it's a foot-gun on PPAP — return 0 without touching the fd).
- handle 5+: `sys_close(fd)`, set slot to -1.
- invalid handle: error 6, CF=1.

**Tests (added to `test_msdos.c`):**
- `create_open_close`: `CREATE "C:\D2.TXT"` → handle ≥ 5 → `CLOSE` → `OPEN` same path for read → handle ≥ 5 → `CLOSE` → verify the host-visible file exists at `{exec_dir}/D2.TXT`.
- `open_missing`: `OPEN` a non-existent path → AX=2, CF=1.
- `bad_handle_close`: `CLOSE` handle 17 → AX=6, CF=1.
- `double_close`: `CREATE` + `CLOSE` + `CLOSE` → second close → AX=6.
- `invalid_drive`: `OPEN "Y:\FOO"` → AX=15, CF=1.

#### D-2d: AH=3Fh READ / AH=40h WRITE

`dos_read` (AH=3Fh):
- BX = handle, CX = byte count, DS:DX = buffer (in the user segment).
- `sys_read(fd, dx_offset, cx)` — DX is already a user-segment offset,
  so it goes straight through `proc_user_ptr_to_page_ref`.  No PSP
  staging needed (this is the natural buffer convention; D-1's
  staging was only for kernel-side single-byte values).
- Return: AX = bytes actually read.

`dos_write` (AH=40h):
- Same shape, calls `sys_write`.

Both must cap CX at 0xFFFF (already 16-bit) and propagate short reads
without an error (DOS treats 0-byte read as EOF, not error).

**Test:** the round-trip — create+write "DOS_RW" + close + open + read
+ verify + close + delete.  Plus a "buffer near segment end" case
(DS:DX = 0xFE00, count = 256) to exercise the page-walk loop.

#### D-2e: AH=41h DELETE / AH=42h LSEEK

`dos_delete` (AH=41h):
- DS:DX = path → `sys_unlink`.  No handle involved.

`dos_lseek` (AH=42h):
- BX = handle, AL = whence (0/1/2 → SEEK_SET/CUR/END),
  CX:DX = 32-bit offset (CX = high word, DX = low word).
- `sys_lseek(fd, ((int32_t)cx << 16) | dx, whence)`.
- Return: 32-bit new position split into DX:AX (DX = high, AX = low).

The CX:DX → int32 packing and back is the only fiddly bit; isolate it
in a helper to keep the dispatcher tidy.

**Test:** write 16 bytes, seek to 4 from start, read 4, verify.  Seek
to -2 from end, read, verify.  Seek with bad whence → AX=1 CF=1.

#### D-2f: Error mapping pass

Audit every D-2 handler against §4.5:
- Negative `sys_*` returns become `(uint16_t)-rc → DOS errno` via a
  small lookup function (ENOENT→2, ENOTDIR→3, EMFILE→4, EACCES→5,
  EBADF→6, ENOMEM→8, EEXIST→80; default → 1 invalid function).
- Set `regs->flags |= 0x0001` (CF) on error, clear on success.
- Confirm `regs->ax` carries the DOS error code (not the raw negated
  errno) on every error path.

**Phase verification:**

1. `test_msdos` grows by ~5 sub-tests covering the round-trip,
   page-boundary buffer, seek, double-close, bad-handle, and
   invalid-drive paths.
2. All 16/16 PPAP tests on pcxt still pass; no regressions on
   qemu_arm/m68k/rv32.

**Out of scope for D-2** (move to D-4 or later):
- AH=39h/3Ah/3Bh — mkdir/rmdir/chdir.
- AH=44h IOCTL (any sub-function).
- AH=45h/46h DUP / DUP2.
- AH=4Eh/4Fh FindFirst / FindNext.
- AH=56h RENAME (already in §4.2 Phase 2 table).
- Inheritable-handle accounting in PSP[0x32] (deferred until EXEC in D-5).

### Phase D-3: Directory + handle housekeeping — DONE

**Goal**: Common DOS file/directory housekeeping calls work, so DOS
programs can navigate, manipulate the filesystem, and reuse handles.

1. ✓ AH=39h MKDIR / AH=3Ah RMDIR — `sys_mkdir` / `sys_rmdir` via
   `dos_resolve_user_path` + kmem scratch.
2. ✓ AH=3Bh CHDIR / AH=47h GETCWD — per-DOS-process state in
   `dos_proc_t.cwd_c` / `cwd_z` (independent of the kernel cwd).
   CHDIR verifies the target exists via a `sys_open` round-trip and
   updates `current_drive`.
3. ✓ AH=45h DUP / AH=46h DUP2 — wrap `sys_dup`.  DUP2 closes the
   destination's underlying fd before installing the duplicate.
4. ✓ AH=56h RENAME — uses both scratch slots
   (`DOS_PATH_SCRATCH_OFF` and `DOS_PATH_SCRATCH2_OFF`) so both
   resolved paths can be passed to `sys_rename`.

**Verification**: `test_msdos` gains `test_mkdir_rmdir`, `test_dup_handle`,
and `test_rename` (43/43 sub-tests on pcxt).

**Out of scope for D-3** (kept for later phases):
- AH=43h GET/SET attributes (need a stat-driven mode → DOS attribute
  byte mapping).
- AH=06h direct console I/O (needs non-blocking read semantics).
- AH=4Eh/4Fh FindFirst / FindNext (DTA layout + 8.3 pattern matching).
- AH=2Ah/2Ch real date/time (no RTC source; current stubs return
  fixed values).
- AH=25h/35h interrupt vector get/set (emulated IVT).

### Phase D-4: .EXE Loading — DONE

**Goal**: Multi-segment DOS .EXE programs run.

1. ✓ **D-4a** — MZ header parse + zero-relocation .EXE load.  Detects
   MZ/ZM signature, reads the 28-byte header, allocates a contiguous
   run sized for PSP + image + min_alloc, streams the image into
   `(proc_seg+0x10):0`, builds initial HW+SW frame from `init_cs/ip`
   and `init_ss/sp`.
2. ✓ **D-4b** — Apply MZ relocations.  For each `(offset, seg)` entry
   in the table, patch the 16-bit word at `(load_seg + seg):offset` by
   adding `load_seg`.  Cross-page-safe via `dos_read/write_run_bytes`.
3. ✓ **D-4c** — Header hardening (`page_count == 0`, `header_size < 2`)
   plus a multi-segment DS test that proves the bridge resolves DS:DX
   for any DS within the proc-image run.

**Verification**: `test_msdos` 53/53 on pcxt (was 43/43 after D-3).
Hand-assembled MZ blobs cover exit-code, hello-world, JMP-FAR with
relocation, and DS != CS != PSP.

### Phase D-5: Memory and Process Management

Split into D-5a (memory) and D-5b (process) since the immediate need
is for crt0s of real .EXE programs to find a valid MCB and resize their
allocation; spawning child processes is independent.

#### Phase D-5a: MCB chain and memory functions

1. ✓ **D-5a.1** — Run layout shifted by one paragraph so a 16-byte
   MCB header sits at `proc_seg - 1` for every DOS process.  PSP and
   image addresses are still proc-seg-relative (PSP:0 = proc_seg:0,
   image at proc_seg:0x100 / load_seg:init_ip), but the in-run offsets
   are now MCB-aware.  `DOS_SEG_PAGES` floor bumped 16 → 17 so a .COM
   still gets the full 64 KB window past the MCB.  See §3.0 for the
   layout in detail.
2. **D-5a.2** — AH=4Ah Resize Block.  Validate the MCB at `(ES-1):0`,
   split into `'M'(BX)` + `'Z'(remainder, owner=0)`.  On failure, set
   CF, AX=8, BX=max-available.
3. **D-5a.3** — AH=48h Allocate / AH=49h Free + chain coalescing.

#### Phase D-5b: Process functions

1. INT 21h AH=4Bh (EXEC — load and execute child program).
2. INT 21h AH=4Dh (get child return code).

**Verification**: A parent .COM/.EXE launches a child and retrieves
its exit code.

### Phase D-6: Console and Keyboard

**Goal**: Interactive DOS programs work.

1. INT 21h AH=01h/06h/08h/0Ah (character input functions).
2. INT 10h AH=02h/0Eh (cursor positioning, teletype output).
3. INT 16h AH=00h/01h (keyboard read/check).

**Verification**: A simple DOS text editor or menu-driven program works
interactively.

### Phase D-7: FCB Operations and Compatibility

**Goal**: DOS 1.x programs using FCBs work.

1. INT 21h AH=0Fh–28h (FCB file operations).
2. Directory search (FindFirst/FindNext via AH=4Eh/4Fh).
3. Remaining misc functions (AH=19h, 25h, 2Ah, 2Ch, 30h, 35h, etc.).

**Verification**: Classic DOS utilities (EDLIN, DEBUG) run.

### Phase D-8: Trace Integration

**Goal**: `trace --subsys` and `pdb` work with DOS programs.

1. Add `PPAP_TRACE_ABI_DOS_INT21` events.
2. INT 21h function name decoder in trace tool.
3. 8086 register display in pdb.

**Verification**: `trace --subsys hello.com` shows decoded INT 21h calls.

---

## 11. Testing

### 11.1 Test Programs

Ship a set of minimal DOS .COM test programs in `tests/dos/`:

```
tests/dos/
  hello.com           — AH=09h print + AH=4Ch exit
  fileio.com          — Create, write, read, close, delete
  args.com            — Print command tail from PSP
  memory.com          — AH=48h alloc + AH=49h free
  exec.com            — AH=4Bh launch child + AH=4Dh wait
```

These are assembled from source using NASM and included in the test
suite as binary blobs (similar to test_x68k and test_cpm patterns).

### 11.2 Integration Test

A `test_dos` test case exercises the bridge via the standard PPAP test
runner.  It is similar in structure to `test_cpm`:

```c
static int test_dos(void) {
    /* Spawn hello.com via execve, verify output */
    /* Spawn fileio.com, verify file created and read back */
    /* Spawn exec.com, verify child return code */
    return 0;
}
```

---

## 12. Risks and Open Questions

### 12.1 Case Sensitivity

PPAP filesystems (tmpfs, UFS) are case-sensitive.  DOS is
case-insensitive.  Supporting that properly needs case-folding
directory lookups, which adds overhead and complicates the bridge.

**Current policy (D-2 and earlier): case-sensitive lookup.**  The
bridge passes DOS paths through `dos_resolve_path` with backslash→slash
and drive-letter→mount substitution only.  No case folding.  DOS
programs that emit lowercase names see PPAP files as-is; programs that
uppercase (typical for .COM utilities) need matching uppercase files
in the image.  Test programs stage their own files so this is fine for
the regression suite.

Case-folding lookup (uppercase-on-write + case-insensitive scan on
read) is deferred; it will become necessary once we try to run real
DOS applications that assume `FILE.TXT == file.txt`.

### 12.2 i8086 eCPU Performance

The i8086 eCPU is a software interpreter running on ARM or m68k.  Complex
DOS programs with heavy computation may be slow.  For the initial target
(simple utilities, text adventures, BASIC interpreters), this should be
acceptable.

### 12.3 INT 21h Coverage

MS-DOS has ~100 INT 21h functions, many with sub-functions.  Full
compatibility is impractical.  The phased approach covers the most-used
subset.  Programs using exotic INT 21h functions will fail gracefully
(return "invalid function" error code).

### 12.4 TSR Programs

Terminate-and-Stay-Resident programs (INT 21h AH=31h, INT 27h) are
complex to support correctly.  They require keeping the program's memory
allocated while the parent process resumes.  This is deferred to extended
goals.

### 12.5 COMMAND.COM

The DOS subsystem does not include a COMMAND.COM equivalent.  Programs
are launched from the PPAP shell.  A minimal COMMAND.COM with `.BAT`
file support could be added as an extended goal.

### 12.6 ia16 gotchas surfaced during D-1

Two ia16-specific traps caught while bringing test_msdos up; future
INT 21h handlers should keep them in mind.

- **Kernel buffers cannot be passed straight to sys_read/sys_write.**
  The user-pointer translator on ia16 (`arch_user_ptr_to_page`) treats
  its `user_ptr` argument as a 16-bit segment-relative offset, not a
  flat address.  A kernel-stack pointer fed in (e.g. `&local_byte`)
  resolves to a random offset inside the user's segment — not the
  byte we intended.  Single-byte INT 21h handlers must stage through
  the .COM's PSP (the bridge uses `PSP[0x60]` as a 1-byte scratch slot
  via `dos_io_putc/getc`) and then call `sys_write`/`sys_read` with
  the *user-segment* offset.  Flat-memory targets are not affected.
- **Don't trust the user-pushed DS for source/destination segments
  unless the contract truly says DS:DX/DS:SI.**  ia16-elf-gcc treats
  DS as a scratch register, so any ia16-compiled INT 21h caller can
  arrive with arbitrary DS.  Hand-assembled .COMs follow the DOS
  contract and set DS=PSP segment, so bridge handlers can use `regs->ds`
  for the AH=09h/0Ah-style "DS:DX is the buffer" calls.  For internal
  scratch staging from the kernel side, derive the proc segment from
  the PCB (`proc_page_backed_base(current)`) instead.

---

## 13. Dependency Graph

```
i8086 eCPU (docs/proposals/i8086_ecpu.md)        ← future, not on critical path
  └─→ D-1 (.COM + minimal INT 21h)            ✓ done
        └─→ D-2 (file I/O)                    ✓ done
              └─→ D-3 (directory + handles)   ✓ done
                    └─→ D-4 (.EXE loading)    ✓ done (a/b/c)
                    └─→ D-5a (memory + MCB)   ▸ in progress
                    │     └─→ D-5b (EXEC + wait)
                    └─→ D-6 (console + keyboard)
                          └─→ D-7 (FCB + compat)
                                └─→ D-8 (trace integration)
```

The native ia16 path (current) does not need the i8086 eCPU at all —
real-mode 8086 code runs on the host CPU, and INT 21h is trapped by
`dos_trap.S`.  The eCPU is only required if/when we want DOS programs
to run on ARM, m68k, RISC-V, or Xtensa hosts.

---

## 14. Related Documentation

- [docs/proposals/i8086_ecpu.md](i8086_ecpu.md) — i8086 eCPU emulator
- [docs/targets/ia16.md](../targets/ia16.md) — IBM PC target port (ia16)
- [docs/user/trace.md](../user/trace.md) — Trace and debug subsystem
- [docs/kernel/syscall.md](../kernel/syscall.md) — System call reference
