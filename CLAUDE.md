@AGENTS.md

# Claude Code specifics
- Use the Task/subagent feature for broad searches or log analysis so the main context stays small.
- Use a todo list that mirrors the phases in `docs/SPEC.md`.
- Run long QEMU runs with timeouts (`timeout 60 make run` or `make test`) and run them in the
  background if they would otherwise block. Never leave QEMU running when you finish a step.
- Prefer targeted file reads (offsets/ranges) over reading entire large files.