#!/bin/sh
# Boot the ISO headless, wait, then dump the QEMU framebuffer to a PPM file.
# Used both interactively and by `make test` to prove that the graphical
# console really is being drawn.
#
# Usage: tools/shot.sh <iso> <out.ppm> [seconds] [extra qemu args...]

set -eu

ISO=${1:?usage: shot.sh <iso> <out.ppm> [seconds] [qemu args...]}
OUT=${2:?usage: shot.sh <iso> <out.ppm> [seconds] [qemu args...]}
WAIT=${3:-8}
if [ $# -ge 3 ]; then shift 3; else shift 2; fi

case "$OUT" in
    /*) ;;
    *) OUT="$PWD/$OUT" ;;
esac
rm -f "$OUT"

ACCEL="-accel tcg,thread=multi"
if [ -r /dev/kvm ]; then
    ACCEL="-accel kvm"
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/minx-shot.XXXXXX")
SERIAL="$TMP/serial.log"

cleanup() {
    [ -n "${SHOW_SERIAL:-}" ] && cat "$SERIAL"
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

# The monitor reads from stdin, so the commands below go straight into QEMU.
{
    sleep "$WAIT"
    printf 'screendump %s\n' "$OUT"
    sleep 1
    printf 'quit\n'
} | timeout $((WAIT + 60)) qemu-system-x86_64 \
        -m "${MEM:-512M}" -smp "${SMP:-4}" \
        -no-reboot -boot d \
        -drive "file=$ISO,format=raw,if=none,id=minxcd" \
        -device ide-cd,drive=minxcd \
        -serial "file:$SERIAL" \
        -vga std -display none $ACCEL \
        -monitor stdio "$@" >"$TMP/monitor.log" 2>&1 || true

if [ ! -s "$OUT" ]; then
    echo "shot.sh: screendump produced no file" >&2
    tail -20 "$TMP/monitor.log" >&2
    exit 1
fi

echo "shot.sh: wrote $OUT ($(wc -c < "$OUT") bytes)"