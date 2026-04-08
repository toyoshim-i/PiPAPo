# i16 vfork/init regression investigation

Date: 2026-04-08
Target: pcxt (i16)

## Problem statement

Init appeared to run twice and shell did not come up reliably after recent vfork-related changes.

Observed symptoms during investigation:
- `CRT0` appeared multiple times.
- `init: step0 pid=1` appeared again after a spawn attempt.
- Later, parent-side vfork return became healthy (`ret=2 self=1`) but shell still did not stay running.

## Hypotheses investigated

1. Init loader bug at page boundary (second page not loaded)
- Why considered: earlier invalid opcode and boundary-looking symptoms.
- How checked: kernel diagnostics around init frame/page reads.
- Result: rejected.

2. Incorrect initial user frame for init (bad SS:SP/IP/CS/argc/argv)
- Why considered: repeated init startup can come from malformed first frame.
- How checked: decoded launch frame from `exec_user_ss`/`exec_user_sp` in kernel logs.
- Result: rejected.

3. Parent vfork frame save/restore corruption
- Why considered: parent restarted unexpectedly.
- How checked: added save and restore diagnostics:
  - `VFORK: ... ip=...`
  - `VFORK_RST: ... ip=...`
- Result: rejected as primary cause for the first regression point.

4. vfork parent resumed from wrong return path due to child clobbering parent stack
- Why considered: on i16, child was sharing parent user stack and could overwrite normal C call frames below the saved interrupt frame.
- How checked: compared behavior with private child stack-page copy fix.
- Result: supported.

5. Remaining shell-start failure after parent return fix is in child exec/runtime path
- Why considered: parent showed healthy return (`ret=2 self=1`) but shell still did not stay up.
- How checked: added init child/exec/wait lifecycle logs.
- Result: still under active investigation.

## Found facts

1. Init launch frame is valid
- Kernel diagnostics showed expected initial state (IP=0, segment setup, argc/argv layout coherent).

2. vfork frame save and restore matched
- Captured values:
  - `VFORK: usp=0x00001e70 uss=0x00002200 rel=0x00001e70 pg=0x00000023 off=0x00000e70 ip=0x00000070`
  - `VFORK_RST: usp=0x00001e70 uss=0x00002200 ip=0x00000070`
- Interpretation: saved and restored GP+IRET frame IP matched.

3. Parent vfork return later became healthy
- Captured value: `ret=2 self=1`
- Interpretation: parent resumed as PID 1 and received child PID 2.

4. i16 child stack-sharing was unsafe in this path
- Child executing pre-exec userspace code could overwrite parent's live stack frames.
- A private copied stack-page strategy for i16 child was implemented to eliminate this overwrite class.

5. Shell still not staying up after parent-side return fix
- No `execve failed` line appeared from init instrumentation at one point, indicating failure mode is not yet fully pinned.

## Changes made during investigation

- Added i16 kernel diagnostics for vfork frame save/restore path.
- Added init-side diagnostics around spawn/vfork/child-exec/waitpid.
- Implemented i16 vfork child private stack-page copy to avoid parent stack-frame clobber.

## Current status

- Confirmed regression nature and fixed one key regression vector (parent-side stack corruption/resume path).
- Remaining issue: shell does not remain running; child lifecycle after vfork is still being traced.

## Next checks

1. Capture contiguous log lines from:
- `init: spawn self=...`
- `init: spawned pid=...`
- `init: child pre-exec cmd=...`
- first `init: waitpid ret=... status=...`

2. Decode `waitpid` status to determine whether child exits immediately, is signaled, or exec path is replaced but terminal/session path fails.

3. If child exits quickly, inspect shell entry path and stdio/tty wiring after exec for pcxt.
