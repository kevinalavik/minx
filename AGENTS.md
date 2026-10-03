# AGENTS.md: Instructions for all AI coding agents

Read this fully before doing anything. Full OS requirements are in `docs/SPEC.md`.
This applies to every agent and every model working in this repo.

## Mission
Build a Unix-like x86-64 (amd64) operating system with working SMP, per-process virtual
memory, a VFS with an initrd-backed ramfs, a **graphical (framebuffer) TTY from the very
first boot**, and a full userspace (libc, init, `sh`, coreutils). `make run` must build
everything and boot it in QEMU. Work autonomously; do not ask questions you can answer
yourself. Record decisions in `docs/DECISIONS.md`.

## First steps every session
1. Run `git config core.hooksPath .githooks` (enables the commit/push checks).
2. Read `docs/SPEC.md`, `docs/STATUS.md`, and `docs/THIRD_PARTY.md`.
3. Run `git log --oneline | head -20` and `git status`.
4. Resume at the first phase in STATUS.md not marked VERIFIED. Never restart finished work.

## Third-party code is allowed (your choice)
You are free to pick whatever existing, well-known open-source components make the project
better and faster to finish. Before writing a subsystem from scratch, **search for an
existing solution** (web search, GitHub, package registries) and decide deliberately.
Candidates to evaluate (not a requirement, not exhaustive):
- Bootloader: **Limine** (recommended), GRUB multiboot2, others.
- Terminal emulation on the framebuffer: **Flanterm** (pairs with Limine's framebuffer),
  plus a bitmap font such as Terminus/Spleen PSF.
- Allocators: **dlmalloc**, liballoc, TLSF, or your own slab.
- ACPI: **uACPI**, ACPICA, or your own MADT parser.
- libc: **mlibc**, musl, newlib, or your own. mlibc needs a sysdeps port to your syscalls.
- Printf: nanoprintf / eyalroz printf, etc. Filesystems: lwext4, FatFs, etc.
- Userland tools: toybox, busybox, sbase/ubase, or your own, for coreutils, `sh` and an editor.
  The shell, init and an editor must exist and work whichever way you obtain them.
Rules for third-party code:
- Pin exact versions/commits. Either vendor under `third_party/<name>/` (committed) or have
  the Makefile fetch them automatically with a checksum or commit pin. No manual steps.
- Record every component in `docs/THIRD_PARTY.md`: name, version/commit, URL, license,
  what it is used for, and why it was chosen. Keep license files intact.
- Prefer permissive licenses (BSD/MIT/ISC/Zlib/public domain). GPL or other copyleft code is
  allowed if you note the implications in `docs/THIRD_PARTY.md`.
- Wrap third-party code behind thin interfaces of your own so it can be swapped later.
- Do not copy-paste Linux/BSD kernel source into the kernel without recording it in
  `THIRD_PARTY.md`.

## Graphical TTY from the start
- The system must show a real **graphical console on the framebuffer from the first boot
  phase** (Phase 1), not only serial. Use the bootloader's framebuffer plus a terminal
  emulator (e.g. Flanterm) or your own renderer with a bitmap font.
- All kernel logs (`kprintf`, panic output) go to both the framebuffer console and COM1 serial.
- `make run` opens a QEMU window by default (`-vga std`, display backend gtk/sdl auto-detected).
  `GUI=0` runs headless with serial on stdio; `make test` always runs headless.
- Keyboard input (PS/2 first; USB/virtio-input optional) must feed the TTY layer early, so
  you can type into the graphical console as soon as the input stack exists. The shell runs on
  the graphical console by default.
- The console must handle ANSI/VT100 escape sequences, scrolling, colors and cursor.

## Non-destructive rule (CRITICAL)
**You are NOT allowed to remove committed code.** Concretely:
- Never delete committed files or directories, never delete functions/features/tests, never
  remove committed lines wholesale, and never "clean up" by removing things.
- Never run `git revert`, `git reset --hard` past a commit, `git rebase` on committed
  history, `git filter-branch`/`filter-repo`, `git commit --amend` on a commit that was
  already pushed or that you later built on, `git checkout -- <file>` or `git restore` to
  discard committed work, or force-push.
- Allowed: adding code, extending code, fixing bugs by editing lines in place, refactoring
  while keeping behavior and all tests passing, and renaming or moving files with `git mv`
  (history preserved, functionality intact).
- If something is wrong or obsolete, **do not delete it**: fix it, or leave it and add the
  replacement beside it, selected by a Makefile/config option, and note it as deprecated in
  `docs/DECISIONS.md`.
- Tests are code: never delete or weaken a test. If a test fails, fix the cause.
- Uncommitted scratch files that you created yourself and never committed may be deleted.
- If you believe removal is truly unavoidable, do not do it. Record the reason in
  `docs/STATUS.md` under "Needs human decision" and work around it.
- `tools/check-no-deletions.sh` checks this; run it before pushing.

## Git & GitHub rules
- Work on the `main` branch unless the repo already uses another default branch.
- Use the existing `origin` remote (`git remote -v`). Do not change or add remotes.
- Commit early and often: one logical change per commit, imperative messages prefixed by
  subsystem, e.g. `mm: add buddy allocator`, `console: flanterm framebuffer terminal`.
- **Pushing never blocks progress.** Try `git push origin HEAD` after each verified phase
  (and whenever convenient); if it fails or is slow, continue working and retry later. Do not
  wait for it, do not stop because of it. Run it in the background if your tool supports that.
- Never force-push, rewrite published history, delete branches, or skip hooks (`--no-verify`).
  Never put tokens/credentials in files, remotes, or commit messages.
- Do not commit build output (`build/`, `*.o`, `*.iso`, `*.elf`, `*.d`, qemu logs) or
  auto-fetched third-party checkouts. Keep `.gitignore` updated. (Deliberately vendored
  code under `third_party/` is committed.)
- Only commit code that builds. Do not commit something that breaks `make run` or `make test`.

### Co-author trailer (mandatory on every commit)
Every commit message must end with a `Co-authored-by` trailer naming **the AI model that
wrote the change**, not the tool/harness you run in. Example: a commit written by a model
called "Example Model 1" through any CLI tool:

    Co-authored-by: Example Model 1 <noreply@example.com>

- Use your actual model name and version as you know it (for example the model ID from your
  system prompt or configuration). Never use a tool name such as "Claude Code", "Codex",
  "OpenCode", "Freebuff" or "Codebuff" as the name.
- Email: use the model vendor's no-reply address if you know it (e.g. `noreply@anthropic.com`
  for Anthropic models, `noreply@openai.com` for OpenAI models); otherwise `noreply@<vendor-domain>`.
- If a different model continues the work in a later session, it uses its own name.
- If you truly cannot determine your model name, write
  `Co-authored-by: Unknown AI Model <noreply@invalid>` and keep going.
- Keep the trailer after a blank line at the end of the message, one trailer per model.
- If your tool adds its own automatic co-author line naming the tool, remove or disable it.

## README.md rule
- **Do not modify, reformat, delete, or reorder anything in `README.md`.**
- Single exception: you may **add** content under the **first section** (first heading and
  its body), such as a short build/run blurb. Only append there; never alter existing lines
  or touch other sections.
- If the README is missing or has no section, do not create or restructure it. Put all other
  docs in `docs/` (ARCH, MEMORY, LOCKING, SYSCALLS, STATUS, DECISIONS, THIRD_PARTY).

## Build and tooling rules
- Host tools: gcc or clang, GNU make, binutils, nasm or GNU as, xorriso, QEMU
  (`qemu-system-x86_64`), git. Prefer `-ffreestanding` with the host compiler; if a cross
  toolchain is needed, detect it and fail with a clear message. Install missing tools if permitted.
- `make run` is the contract: clean checkout -> build -> boot to a shell on the graphical
  console in QEMU (`SMP=4` default, `-m 512M`, serial also on stdio, KVM if available else TCG).
  Also provide `all`, `debug`, `test`, `iso`, `clean`, `distclean`, `help`.
  (`clean`/`distclean` only remove build outputs, never source.)
- Parallel-safe (`-j`), incremental (`-MMD -MP`), warning-free with `-Wall -Wextra` for your
  own code in the final state (third-party code may use its own flags).

## Working method
Follow the phases in `docs/SPEC.md`. After each phase:
1. Build and actually run it in QEMU. Serial output and a screenshot are the source of truth
   (QEMU monitor `screendump` can verify the framebuffer in headless tests).
2. Run that phase's tests plus all earlier tests (no regressions).
3. Update `docs/STATUS.md` (mark VERIFIED with the evidence/command used).
4. Commit (with the co-author trailer) and try to push; continue regardless of push outcome.
Do not start the next phase until the current one is verified by running it. On crashes use
serial logging, `-d int,cpu_reset -D qemu.log`, GDB (`make debug`) and bisection. Do not
stack speculative fixes.

## Honesty rules
- No stubs masquerading as features. Never claim something works unless you ran it.
- List anything unfinished or flaky in `docs/STATUS.md`.

## Code rules
- C11/GNU C plus assembly (other languages allowed for third-party components).
- Kernel flags: `-mno-red-zone -mcmodel=kernel -mno-sse -mno-mmx -fno-pic -fno-pie
  -fno-stack-protector -ffreestanding` (third-party kernel components may need adjustments).
- Validate every user pointer in syscalls (copy_from_user/copy_to_user with fault handling).
- Every shared structure has a documented lock; document ordering in `docs/LOCKING.md`.
- Support 1-64 CPUs and 128 MiB to multiple GiB of RAM; no hard-coded CPU counts.

## Context management
Keep `docs/STATUS.md` current at all times (current phase, verified items, next steps, known
bugs, exact test commands). A new session, possibly a different model, must be able to
continue from STATUS.md alone. Do not paste whole source files into chat; keep reports brief.

## Stop condition
Stop only when the Definition of Done in `docs/SPEC.md` is fully met, or every reasonable
approach is exhausted. In the latter case report precisely what remains broken and why.
