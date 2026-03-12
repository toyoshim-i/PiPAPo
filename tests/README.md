# PPAP Tests

See [docs/getting_started/testing.md](../docs/getting_started/testing.md) for the full testing guide:
how to run tests, how to write new tests, build flags, frameworks,
and architecture constraints.

## Directory layout

```
tests/
  host/           Host-native unit tests (system compiler, no ARM/m68k)
  kernel/         On-target kernel integration tests (ktest.c)
  user/           On-target user-space tests (standalone ELF binaries)
```
