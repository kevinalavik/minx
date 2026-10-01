# Project status

## Current phase
Phase 2 — CPU core. Phase 1 is VERIFIED; the initial repository had no OS implementation or
prior STATUS.md.

## Verified phases
1. Toolchain & boot — VERIFIED. Evidence: `make clean && make -j4 test && make -j4 all`
produced a BIOS bootable ISO and the TCG QEMU self-test printed `MINX: phase 1 boot OK` and
`MINX: self-test PASS`; `make -j4 test SMP=1` also passed. `timeout 5s make run` booted the
normal ISO under KVM and printed the normal serial startup message (timeout was expected because
the kernel currently halts).

## Phase checklist
1. Toolchain & boot — VERIFIED. Limine BIOS boot, COM1 serial, panic path, and headless test.
2. CPU core — NOT STARTED.
3. Memory — NOT STARTED.
4. SMP — NOT STARTED.
5. Scheduler & kernel threads — NOT STARTED.
6. Userspace entry — NOT STARTED.
7. Processes — NOT STARTED.
8. VFS + ramfs + initrd + devfs — NOT STARTED.
9. TTY + libc + sh — NOT STARTED.
10. Coreutils + procfs — NOT STARTED.
11. Hardening — NOT STARTED.
12. Polish — NOT STARTED.

## Git delivery
- Phase 1 commit `7707359` is local. Push was attempted twice: first failed with GitHub HTTP 503,
  retry failed because this environment cannot read GitHub credentials (`could not read Username`).
  Retry pushing at the next phase boundary when credentials/network are available.

## Known gaps
- CPU initialization, exceptions, memory management, SMP startup, scheduler, userland and shell
  remain unimplemented.
- Phase 1 normal kernel deliberately halts after the serial startup message.
- The automated boot test observes the self-test markers then terminates QEMU after a timeout;
  successful boot output is captured and checked.

## Commands
Phase 1: `make clean && make -j4 test && make -j4 all`; normal serial boot: `timeout 5s make run`.
Next phase: implement CPU core features from `docs/SPEC.md` and verify in QEMU before proceeding.
