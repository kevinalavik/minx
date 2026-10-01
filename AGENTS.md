# AGENTS.md — Instructions for all AI coding agents

This file is for any agent working in this repo (Claude Code, Codex, OpenCode, Freebuff, etc.).
Read it fully before doing anything. The full OS requirements are in `docs/SPEC.md`.

## Mission
Build, from scratch, a Unix-like x86-64 (amd64) operating system with working SMP, per-process
virtual memory, a VFS with an initrd-backed ramfs, and a full userspace (libc, init, `sh`,
coreutils). You write everything yourself except the bootloader (Limine recommended).
`make run` must build everything and boot it in QEMU. Work autonomously; do not ask questions
you can answer yourself. Record decisions in `docs/DECISIONS.md`.

## First steps every session
1. Read `docs/SPEC.md` and `docs/STATUS.md` (create STATUS.md if missing).
2. Run `git log --oneline | head -20` and `git status`.
3. Resume at the first phase in STATUS.md that is not marked VERIFIED. Never restart finished work.

## Git & GitHub rules
- Work on the `main` branch unless the repo already uses another default branch.
- Use the existing `origin` remote. Check with `git remote -v`. Do not change or add remotes.
- Commit early and often: one logical change per commit, imperative messages, e.g.
  `mm: add buddy allocator`, `sched: per-CPU run queues`. Prefix with the subsystem.
- **Push after every verified phase** (and at least every few commits): `git push origin HEAD`.
- Never force-push, rewrite published history, or delete branches. Never skip hooks.
- If push fails (auth/network), keep committing locally, note it in `docs/STATUS.md`, and retry at
  the next phase boundary. Never put tokens or credentials into files, remotes, or commit messages.
- Do not commit build output (`build/`, `*.o`, `*.iso`, `*.elf`, `*.d`, qemu logs) or fetched
  third-party sources (e.g. the Limine checkout). Maintain a `.gitignore` for these.
  The Makefile must fetch the pinned bootloader automatically.
- Only commit code that builds. Do not commit something that breaks `make run` or `make test`.

## README.md rule (important)
- **Do not modify, reformat, delete, or reorder anything in `README.md`.**
- The single exception: you may **add** new content under the **two first sections** of the README
  (the first heading and its body), e.g. a short "Build & run" or "Project overview" blurb.
  Only append there; never alter existing lines, and never touch other sections.
- If the README does not exist or has no section, do not create or restructure it. Put all
  other documentation in `docs/` (ARCH.md, MEMORY.md, LOCKING.md, SYSCALLS.md, STATUS.md,
  DECISIONS.md).

## Build and tooling rules
- Host tools: gcc or clang, GNU make, binutils, nasm or GNU as, xorriso, QEMU
  (`qemu-system-x86_64`), git. Use `-ffreestanding`; avoid needing a custom cross-compiler,
  or detect one and fall back with a clear error message.
- `make run` is the contract: clean checkout -> build -> boot to a shell in QEMU
  (`SMP=4` default, `-m 512M`, serial on stdio, headless by default, `GUI=1` for a window,
  KVM if available else TCG). Also provide `all`, `debug`, `test`, `iso`, `clean`, `distclean`, `help`.
- Build must be parallel-safe (`-j`), incremental (`-MMD -MP`), and eventually warning-free
  with `-Wall -Wextra`.
- If a tool is missing, try to install it (apt etc.) if permitted. Otherwise pick a fallback,
  record it in `docs/DECISIONS.md`, and continue.

## Working method
Follow the phases in `docs/SPEC.md`. After each phase:
1. Build and actually run in QEMU. Serial output is the source of truth.
2. Run that phase's tests, and all earlier tests (no regressions).
3. Update `docs/STATUS.md` (mark phase VERIFIED with the evidence/command used).
4. Commit and push.
Do not start the next phase until the current one is verified by running it.
When something crashes: use serial logging, `-d int,cpu_reset -D qemu.log`, GDB (`make debug`),
and bisect against the last good commit. Do not stack speculative fixes.

## Honesty rules
- No stubs masquerading as features. Never claim something works unless you ran it.
- Never delete or weaken a failing test to make it pass.
- List anything unfinished or flaky in `docs/STATUS.md`.

## Code rules
- C11/GNU C plus assembly. Kernel flags: `-mno-red-zone -mcmodel=kernel -mno-sse -mno-mmx
  -fno-pic -fno-pie -fno-stack-protector -ffreestanding`.
- Validate every user pointer in syscalls (copy_from_user/copy_to_user with fault handling).
- Every shared structure has a documented lock; document ordering in `docs/LOCKING.md`.
- Do not copy code from Linux/BSD/musl/busybox. Follow POSIX and the SysV AMD64 ABI as specs.
- Support 1-64 CPUs and 128 MiB to multiple GiB RAM; no hard-coded CPU counts.

## Context management (long project)
You may run out of context. Keep `docs/STATUS.md` current at all times (current phase, what is
verified, what is next, known bugs, exact commands to test). A new session must be able to
continue from STATUS.md alone. Don't paste whole source files into chat; keep progress reports brief.

## Stop condition
Stop only when the Definition of Done in `docs/SPEC.md` is fully met, or when every reasonable
approach is exhausted. In the latter case, report precisely what remains broken and why.