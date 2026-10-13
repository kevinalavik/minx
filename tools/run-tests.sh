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

# The raw serial log, and the same log made greppable.
#
# The console mirrors its output byte for byte to COM1, which means the log contains every
# escape sequence the console emitted -- and `grep` stops at the first NUL and calls the
# file binary.  It then reports "binary file matches", prints no tally, and the whole suite
# fails for a reason that has nothing to do with the kernel: a run in which the guest
# printed "44 passed, 0 failed" was reported as "the guest printed no tally", and a run
# whose only difference was one stray escape byte was reported as "43 check(s) reported
# PASS" because one `PASS` line had a colour sequence in front of it.
#
# So every check below runs against a cleaned copy: CSI and other escape sequences removed,
# then the control bytes that are not part of any text.  The *guest's own tally* is still
# cross-checked against the count, which is what catches a line the cleaning mangled.
CLEAN=$BUILD/test-serial.clean
# Filled in below, once the guest has finished writing to $LOG.

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

rm -f "$LOG" "$MONLOG" "$SHOT" "$CLEAN"

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

# The greppable copy, made now that the guest has stopped writing.  See the note where
# `$CLEAN` is declared: every check below runs against this, not against the raw log.
sed -e 's/\x1b\[[0-9;?<>=]*[ -/]*[@-~]//g' \
    -e 's/\x1b[()][0-9A-B]//g' \
    -e 's/\x1b[>=]//g' \
    "$LOG" 2>/dev/null | tr -d '\000-\010\013\014\016-\037\177-\377' > "$CLEAN"

echo "run-tests: qemu exited with status $QEMU_STATUS"
echo "----------------------------------------------------------------"
cat "$CLEAN" 2>/dev/null || echo "(no serial output)"
echo "----------------------------------------------------------------"

fail=0

# isa-debug-exit maps the value N the guest wrote to QEMU's exit status (N << 1) | 1:
#   0 -> 1, the machine shut itself down with every check passing
#   1 -> 3, the machine shut itself down with checks failing
#   2 -> 5, the kernel panicked
#
# Panic and "a test failed" are different facts and used to share a code, so a run whose
# only problem was one red check was reported as "the guest reported a panic" -- which
# sends the next person looking for a crash that never happened.  `panic()` writes 2 now
# and the self-test writes its own count; see kernel/lib/panic.c.
case "$QEMU_STATUS" in
    1)   echo "run-tests: guest shut down cleanly" ;;
    3)   echo "run-tests: guest shut down with failing checks" >&2; fail=1 ;;
    5)   echo "run-tests: the kernel panicked" >&2; fail=1 ;;
    124) echo "run-tests: guest timed out after ${TIMEOUT}s" >&2; fail=1 ;;
    *)   echo "run-tests: unexpected qemu exit status $QEMU_STATUS" >&2; fail=1 ;;
esac

if [ ! -f "$LOG" ]; then
    echo "run-tests: the guest produced no serial output at all" >&2
    fail=1
else
    if grep -q 'KERNEL PANIC' "$CLEAN"; then
        echo "run-tests: the kernel panicked" >&2
        fail=1
    fi
    if grep -q '^TEST-READY' "$CLEAN"; then
        :
    else
        echo "run-tests: the guest never reached TEST-READY" >&2
        fail=1
    fi
    if grep -q '^FAIL' "$CLEAN"; then
        echo "run-tests: failing checks:" >&2
        grep '^FAIL' "$CLEAN" >&2
        fail=1
    fi
    passes=$(grep -c '^PASS' "$CLEAN" 2>/dev/null || echo 0)
    echo "run-tests: $passes check(s) reported PASS"
    # The guest also prints its own tally; cross-check the two so a line that
    # the serial mirror mangled cannot quietly turn into a pass.
    summary=$(grep -Eo '^[0-9]+ passed, [0-9]+ failed' "$CLEAN" | tail -1)
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