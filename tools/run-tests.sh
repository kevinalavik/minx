#!/bin/sh
# Automated test runner for `make test`.
#
# Boots the test ISO headless, waits for the guest to publish its results over
# COM1, dumps the framebuffer so the graphical console is verified too, and
# then waits for the guest to power itself off through QEMU's isa-debug-exit
# device.
#
# Exit status: 0 when every check passed, non-zero otherwise.

set -u

BUILD=${BUILD:-build}
ISO=${ISO:-$BUILD/minx-test.iso}
QEMU=${QEMU:-qemu-system-x86_64}
SMP=${SMP:-4}
MEM=${MEM:-512M}
# Hard ceiling for the whole run; a hung guest must not hang the build.
TIMEOUT=${MINX_TEST_TIMEOUT:-420}
LOG=$BUILD/test-serial.log
MONLOG=$BUILD/test-monitor.log
SHOT=$BUILD/test-screen.ppm

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "run-tests: $QEMU not found" >&2
    exit 1
fi

if [ ! -f "$ISO" ]; then
    echo "run-tests: $ISO does not exist" >&2
    exit 1
fi

ACCEL="-accel tcg,thread=multi"
if [ -r /dev/kvm ]; then
    ACCEL="-accel kvm"
fi

rm -f "$LOG" "$MONLOG" "$SHOT"

echo "run-tests: booting $ISO (smp=$SMP mem=$MEM, timeout ${TIMEOUT}s)"

# The monitor shares stdin with the script, so the framebuffer dump is fed in
# from a background writer once the guest announces it is ready.
(
    # Give the bootloader time to get going.
    sleep 3
    # Then wait for the guest to finish its checks, up to ~30 seconds.
    i=0
    while [ "$i" -lt 600 ]; do
        if [ -f "$LOG" ] && grep -q '^TEST-READY' "$LOG" 2>/dev/null; then
            break
        fi
        if [ -f "$LOG" ] && grep -q 'KERNEL PANIC' "$LOG" 2>/dev/null; then
            break
        fi
        i=$((i + 1))
        sleep 0.05
    done
    printf 'screendump %s\n' "$SHOT"
    sleep 1
) | timeout "$TIMEOUT" "$QEMU" \
        -m "$MEM" -smp "$SMP" \
        -no-reboot -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -boot d -drive "file=$ISO,format=raw,if=none,id=minxcd" \
        -device ide-cd,drive=minxcd \
        -serial "file:$LOG" \
        -vga std -display none $ACCEL \
        -monitor stdio > "$MONLOG" 2>&1
QEMU_STATUS=$?

echo "run-tests: qemu exited with status $QEMU_STATUS"
echo "----------------------------------------------------------------"
cat "$LOG" 2>/dev/null || echo "(no serial output)"
echo "----------------------------------------------------------------"

fail=0

# isa-debug-exit maps the value N the guest wrote to QEMU's exit status
# (N << 1) | 1: 1 means the guest shut down cleanly, 3 that it panicked.
case "$QEMU_STATUS" in
    1)   echo "run-tests: guest shut down cleanly" ;;
    3)   echo "run-tests: guest reported a panic" >&2; fail=1 ;;
    124) echo "run-tests: guest timed out after ${TIMEOUT}s" >&2; fail=1 ;;
    *)   echo "run-tests: unexpected qemu exit status $QEMU_STATUS" >&2; fail=1 ;;
esac

if [ ! -f "$LOG" ]; then
    echo "run-tests: the guest produced no serial output at all" >&2
    fail=1
else
    if grep -q 'KERNEL PANIC' "$LOG"; then
        echo "run-tests: the kernel panicked" >&2
        fail=1
    fi
    if grep -q '^TEST-READY' "$LOG"; then
        :
    else
        echo "run-tests: the guest never reached TEST-READY" >&2
        fail=1
    fi
    if grep -q '^FAIL' "$LOG"; then
        echo "run-tests: failing checks:" >&2
        grep '^FAIL' "$LOG" >&2
        fail=1
    fi
    passes=$(grep -c '^PASS' "$LOG" 2>/dev/null || echo 0)
    echo "run-tests: $passes check(s) reported PASS"
    # The guest also prints its own tally; cross-check the two so a line that
    # the serial mirror mangled cannot quietly turn into a pass.
    summary=$(grep -Eo '^[0-9]+ passed, [0-9]+ failed' "$LOG" | tail -1)
    if [ -n "$summary" ]; then
        guest_pass=$(echo "$summary" | cut -d' ' -f1)
        guest_fail=$(echo "$summary" | cut -d' ' -f3)
        echo "run-tests: guest tally: $guest_pass passed, $guest_fail failed"
        [ "$passes" = "$guest_pass" ] || {
            echo "run-tests: PASS count $passes does not match the guest's $guest_pass" >&2
            fail=1
        }
        [ "$guest_fail" = "0" ] || fail=1
    else
        echo "run-tests: the guest printed no tally" >&2
        fail=1
    fi
fi

# The graphical console is a hard requirement, so prove the framebuffer really
# was written instead of trusting the boot log alone.
if [ -s "$SHOT" ]; then
    echo "run-tests: framebuffer dump captured ($(wc -c < "$SHOT") bytes)"
else
    echo "run-tests: no framebuffer dump was produced" >&2
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "run-tests: PASS"
else
    echo "run-tests: FAIL" >&2
fi
exit "$fail"