# Kickoff prompt (paste as the first message in Claude Code, Codex, OpenCode, Freebuff)

You are an autonomous OS engineer. Start by reading, in this order: AGENTS.md (or CLAUDE.md /
knowledge.md if your tool loads those instead, they all point to AGENTS.md), docs/SPEC.md,
docs/STATUS.md, docs/THIRD_PARTY.md. Follow them exactly.

Your task: build the complete Unix-like amd64 OS described in docs/SPEC.md (SMP, virtual memory
with COW fork, VFS with initrd ramfs, graphical framebuffer TTY from the very first boot, libc,
init, sh, coreutils) so that `make run` builds and boots it in QEMU. Research and use existing
third-party components where they help (Limine, Flanterm, dlmalloc, uACPI, mlibc, toybox, etc.),
pin them and document them in docs/THIRD_PARTY.md.

Non-negotiable rules:
1. NEVER remove committed code, files, functions or tests. Fix in place, add beside, or git mv.
2. Every commit ends with a "Co-authored-by: <your MODEL name> <noreply@vendor>" trailer. Use the
   model name, never the tool name (not Claude Code, Codex, OpenCode, Freebuff or Codebuff).
3. Commit in small logical commits and try `git push origin HEAD` after each verified phase, but
   never wait for or get blocked by a push; keep working.
4. Do not touch README.md, except appending under its first section.
5. Verify every phase by actually running it in QEMU (serial output plus framebuffer check)
   before moving to the next. Never claim something works without running it. No stubs
   presented as features.
6. Run `git config core.hooksPath .githooks` first. Keep docs/STATUS.md current so any other
   model can continue from it. If STATUS.md marks phases VERIFIED, continue from the first one
   that is not.

Work autonomously and do not ask me questions. Do not stop until the Definition of Done in
docs/SPEC.md is met. Begin now.
