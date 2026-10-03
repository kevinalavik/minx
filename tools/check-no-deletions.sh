#!/bin/sh
# Fails if HEAD deletes any file that exists in origin's default branch.
# Renames are fine (git detects them), pure deletions are not.
base=$(git rev-parse --verify -q origin/main || git rev-parse --verify -q origin/master)
[ -z "$base" ] && exit 0
deleted=$(git diff --name-only --diff-filter=D -M "$base"...HEAD)
if [ -n "$deleted" ]; then
  echo "BLOCKED: these committed files were deleted (forbidden by AGENTS.md):" >&2
  echo "$deleted" >&2
  echo "Restore them (git checkout $base -- <file>) or move them with git mv." >&2
  exit 1
fi
exit 0
