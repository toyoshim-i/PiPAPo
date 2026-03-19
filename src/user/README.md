# PPAP Userland Applications

Bare-metal (no libc) user-space programs for PiPAPo.

For the full developer guide — compiler flags, linking, memory layout,
musl integration, and porting third-party apps — see:

**[docs/getting_started/userland_dev_guide.md](/docs/getting_started/userland_dev_guide.md)**

## Directory Layout

```
src/user/
  arch/arm_m/    ARM crt0.S, syscall.S, user.ld
  arch/m68k/     m68k crt0.S, syscall.S, user.ld
  syscall.h      C syscall declarations
  push.c         PiPAPo μShell (core interpreter)
  push_line.c    Push line editor (editing, history, completion)
  push.h         Shared push definitions
  init.c         /sbin/init
  getty.c        /bin/getty (serial login)
  hello.c        Example program
  pdb*.c/h       PiPAPo debugger
  trace.c        Tracing utility
  ttyctl.c       Terminal control utility
```

## Adding a New Program

1. Create `src/user/myapp.c`
2. Add `myapp` to `USER_APPS` in `cmake/user.cmake`
3. Build — CMake links `crt0.o + syscall.o + myapp.o → myapp.elf`
