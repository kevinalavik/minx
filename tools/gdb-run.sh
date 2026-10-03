#!/bin/sh
# Start a paused QEMU on the minx ISO and run a batch GDB script against the
# kernel.  Handy wrapper so debugging does not need three terminals.
#
# Usage: tools/gdb-run.sh [gdb-script] [gdb extra args...]

set -eu
cd "$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

SCRIPT=${1:-tools/gdb-step.gdb}
[ $# -gt 0 ] && shift

ISO=${ISO:-build/minx.iso}
ACCEL="-accel tcg,thread=multi"
if [ -r /dev/kvm ]; then
    ACCEL="-accel kvm"
fi

qemu-system-x86_64 -m "${MEM:-512M}" -smp "${SMP:-1}" -no-reboot -boot d \
    -drive "file=$ISO,format=raw,if=none,id=minxcd" \
    -device ide-cd,drive=minxcd \
    -serial "file:/tmp/minx-gdb-serial.log" \
    -vga std -display none $ACCEL -s -S -monitor none >/dev/null 2>&1 &
QPID=$!

cleanup() {
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# QEMU binds the gdbstub port almost immediately; a fixed short wait is more
# reliable than probing the port, because probing means attaching and detaching,
# which leaves the guest stopped for whoever connects next.
sleep 1

# Not `exec`: the EXIT trap must still run so the paused QEMU is not orphaned.
gdb -batch -q -x "$SCRIPT" "$@" build/kernel.elf
STATUS=$?
exit "$STATUS"