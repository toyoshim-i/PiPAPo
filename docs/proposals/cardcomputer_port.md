# M5Stack CardComputer Port — Remaining Work

Open cleanups that block declaring the `xtensa_cc` port complete.
When both items below are green, delete this file.

**Note on runtime-ownership handoff.**  The xtensa_cc port stays
ESP-IDF-managed: ESP-IDF owns the initial memory partition, PMS
policy, watchdog / brownout ISRs, and the `spi_master` transports.
A real bare-metal Xtensa port is intentionally deferred to a future
effort, which can use the current ESP-IDF-managed port as a
reference for context switching, trap handling, and ELF loading.

Items already tracked elsewhere are intentionally absent:

- User-space loader / page-pool sizing → [xtensa_xip.md](xtensa_xip.md).
- microSD bring-up → [sd_card.md](sd_card.md) (sub-goal B).
- Hardware reference → [docs/reference/cardcomputer.md](../reference/cardcomputer.md).
- Xtensa arch + shipped xtensa_cc design notes → [docs/targets/xtensa.md](../targets/xtensa.md).

---

## 1. Romfs staging unification

`scripts/build.sh` open-codes the staging directory layout, ELF
install destinations, and `/bin/sh→push` symlink for xtensa_cc,
while every other target's staging goes through
`cmake/stage_romfs.cmake`.  The two lists drifted at least once
(xtensa_cc was missing `/home`, `/usr`, `/mnt`).

Have xtensa_cc invoke `cmake -P stage_romfs.cmake` too, paired with
extending `stage_romfs.cmake` to handle xtensa_cc's `.xip`/`.xipfix`
artifact ELFs — or dropping them, depending on what
[xtensa_xip.md](xtensa_xip.md) settles on.

---

## 2. Call0-compatible 64-bit math helpers

`calc` is excluded from the xtensa_cc user-app build because it
reaches for `__udivdi3` / `__ashldi3` etc., and the ESP-IDF
toolchain only ships a windowed-ABI `libgcc.a` that would corrupt
registers when called from PPAP's call0 user-space.

Add a small set of call0-compatible 64-bit helpers in
`src/user/lib/` and re-enable `calc` in the xtensa_cc user-app
list.
