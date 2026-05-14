# M33 MPU Full Per-Process Data Protection

Use the 4 remaining MPU regions (4–7) to protect user data pages, heap, and mmap
areas on a per-process basis. Currently only the stack page (Region 2) is
switched per process; data/heap/mmap regions have no MPU coverage.

---

## 1. Problem

The current MPU layout uses 4 of 8 available regions:

| Region | Purpose | Scope |
|--------|---------|-------|
| 0 | Kernel data | Static (priv-only) |
| 1 | Flash XIP | Static (RO all) |
| 2 | Process stack | Per-process (RW all, XN) |
| 3 | Peripherals | Static (priv-only) |

User data pages (`user_pages[]`), heap (`brk`), and `mmap` regions are not
covered. On RP2350 with TrustZone, the NS MPU grants all of NS SRAM as RW to
user mode — so process A can read/write process B's data if it knows the
address. There is no per-process data isolation.

## 2. Design

### 2.1 Dynamic Regions 4–7

Assign MPU regions 4–7 as per-process data regions, reprogrammed on every
context switch alongside Region 2 (stack). Each region covers one contiguous
SRAM range with User RW + XN access.

| Region | Purpose |
|--------|---------|
| 4 | Data region 0 (primary: data segment + heap) |
| 5 | Data region 1 (overflow / mmap) |
| 6 | Data region 2 (mmap) |
| 7 | Data region 3 (mmap) |

Unused data regions are disabled (RLAR.EN = 0 on ARMv8-M, RASR.ENABLE = 0 on
ARMv6-M).

### 2.2 PCB Extension

Add a per-process MPU region descriptor array to `pcb_t`:

```c
#define MPU_DATA_REGIONS 4

typedef struct {
    uint32_t base;   /* region base address (32-byte aligned) */
    uint32_t size;   /* region size in bytes (0 = disabled)   */
} mpu_data_region_t;

/* In pcb_t: */
mpu_data_region_t mpu_data[MPU_DATA_REGIONS];
```

### 2.3 Allocation Strategy — Maximise Contiguity

The key insight is that fewer, larger contiguous regions are better than many
small ones, because we only have 4 data regions. The allocator should try to
keep all of a process's memory in as few contiguous blocks as possible.

**ELF loader (`elf_load`):**
- `page_alloc_contiguous(data_pages)` already allocates the data segment as one
  contiguous block → occupies 1 MPU data region (region 4).
- Stack page is separate (Region 2, unchanged).
- Set `mpu_data[0] = {base, data_pages * PAGE_SIZE}`.

**sys_brk (heap growth):**
- `page_alloc_at()` already extends contiguously after `user_pages[0]`. On
  success, just update `mpu_data[0].size` to cover the expanded range (data
  segment + heap together in one region).
- If `page_alloc_at()` fails (gap in physical pages), the brk expansion fails
  as it does today — no change needed.

**sys_mmap2 (anonymous mappings):**
- After allocating pages, find or create an `mpu_data[]` slot:
  1. Check if the new allocation is contiguous with an existing data region —
     if so, extend that region's size (merge).
  2. Otherwise, find a free `mpu_data[]` slot (size == 0) and assign it.
  3. If no free slot is available, **fail the mmap with `-ENOMEM`**. The caller
     (typically libc) will handle the failure gracefully.

**sys_munmap:**
- If the unmapped range is at the end of a data region, shrink the region.
- If it splits a region in the middle, we cannot represent the hole with one
  region — either leave the region covering the full range (wastes protection
  granularity but is safe), or split into two regions if a free slot exists.
- If the entire region is freed, disable it (size = 0).

**vfork:**
- Child shares parent's `mpu_data[]` (copied by value, same as `user_pages[]`).
- On exec, child gets its own `mpu_data[]` from the new ELF load.

### 2.4 mpu_switch() Update

`mpu_switch()` currently programs Region 2 only. Extend it to also program
regions 4–7 from `next->mpu_data[0..3]`:

```c
void mpu_switch(pcb_t *next) {
    if (!mpu_present) return;

    /* Region 2: stack (unchanged) */
    ...

    /* Regions 4–7: per-process data */
    for (int i = 0; i < MPU_DATA_REGIONS; i++) {
        uint32_t reg = 4u + i;
        if (next->mpu_data[i].size == 0) {
            /* Disable region */
            MPU_RNR = reg;
            MPU_RLAR = 0;  /* ARMv8-M; RASR=0 for ARMv6-M */
        } else {
            uint32_t base = next->mpu_data[i].base;
            uint32_t size = next->mpu_data[i].size;
            /* Program as User RW, XN, WB (same attributes as stack) */
            mpu_set_region(reg,
                base | RBAR_SH(SH_NONE) | RBAR_AP(AP8_RW_ALL) | RBAR_XN,
                RLAR_LIMIT(base, size) | RLAR_ATTR(MAIR_IDX_WB) | RLAR_EN);
        }
    }
    arch_dsb_isb();
}
```

### 2.5 NS MPU Tightening (RP2350)

The current NS MPU Region 1 grants all of NS SRAM (512 KB) as RW to user mode.
With per-process data regions in the Secure MPU, the NS MPU should also be
tightened:

- **Option A (simple):** Keep the NS MPU as-is. The Secure MPU regions 4–7
  already restrict which SRAM the current process can access. NS MPU is a
  second layer — broader is fine since the Secure MPU is the tighter gate.
  However, Secure code accessing NS aliases in syscalls still goes through the
  NS MPU, so it must remain permissive for kernel access.

- **Option B (strict):** Reprogram NS MPU data regions per-process too, using
  the NS alias addresses. This doubles the region programming cost on each
  switch. Only needed if we want defense-in-depth against speculative access
  or Secure-side bugs.

**Recommendation:** Option A — the Secure MPU is the enforcement layer. The NS
MPU remains a broad "NS SRAM is accessible" grant, and per-process isolation
comes from Secure MPU regions 4–7.

### 2.6 ARMv6-M Considerations (RP2040)

ARMv6-M MPU requires power-of-2 region sizes (minimum 256 bytes). A data
segment of e.g. 12 KB cannot be covered by a single region — it must be rounded
up to 16 KB, potentially exposing adjacent memory. This is acceptable:
- The exposed area is within the process's own page pool allocation range.
- Other processes' pages are not within the rounded-up region because
  `page_alloc_contiguous()` allocates from the pool sequentially.

Sub-region disable (SRD) bits can be used to mask out the rounded-up tail,
giving 1/8th granularity within a power-of-2 region. For a 16 KB region, each
sub-region is 2 KB — good enough for most cases.

## 3. Failure Semantics

The critical design choice: **if a memory allocation cannot be represented
within the 4 available data regions, the allocation fails.**

| Operation | Failure condition | Result |
|-----------|-------------------|--------|
| `exec` (ELF load) | data segment > 1 contiguous block | Cannot happen (uses `page_alloc_contiguous`) |
| `sys_brk` | `page_alloc_at()` fails to extend contiguously | Returns unchanged break (existing behaviour) |
| `sys_mmap2` | No free `mpu_data[]` slot and cannot merge | Returns `-ENOMEM` |
| `sys_mmap2` | Contiguous page allocation fails | Returns `-ENOMEM` (existing behaviour) |

This is reasonable because:
- Most processes use 1 data region (data+heap contiguous) + 0–2 mmap regions.
- libc malloc uses brk for small allocations and mmap for large ones; with
  4 data regions, this works well.
- If a process truly needs >4 discontiguous data areas, it has outgrown what a
  4 KB-page, 8-region MPU microcontroller can offer.

## 4. Implementation Steps

### Step 1: PCB and mpu_switch()
- Add `mpu_data[4]` to `pcb_t`.
- Extend `mpu_switch()` to program regions 4–7.
- No functional change yet (all `mpu_data[].size` == 0 → regions disabled).

### Step 2: ELF Loader Integration
- After `page_alloc_contiguous()`, set `mpu_data[0]` to cover the data segment.
- Test: process can access its data; accessing other process's data faults.

### Step 3: sys_brk Integration
- On heap growth, update `mpu_data[0].size` to cover data + heap.
- Test: malloc/free in user programs works; heap accessible.

### Step 4: sys_mmap2 / sys_munmap Integration
- Allocate/free `mpu_data[]` slots on mmap/munmap.
- Fail mmap if no slot available.
- Test: mmap-heavy programs (libc large alloc) work within limits.

### Step 5: NS MPU Review (RP2350 only)
- Verify Option A is sufficient with tests.
- Optionally tighten NS MPU if security audit requires it.

## 5. Cost Analysis

| Aspect | Current | After |
|--------|---------|-------|
| `mpu_switch()` register writes | 2–3 (Region 2 only) | 10–15 (Region 2 + 4–7) |
| Context switch overhead | ~20 cycles for MPU | ~60–80 cycles for MPU |
| PCB size increase | 0 | +32 bytes (4 × {base, size}) |
| Code size increase | 0 | ~100–200 bytes |

The additional ~40–60 cycles per context switch is negligible compared to the
full PendSV handler (~200+ cycles) and the 10 ms time slice (1.33M cycles at
133 MHz).
