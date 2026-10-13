#!/bin/sh
# Boot minx headless and type at it through the QEMU monitor, so the keyboard
# path, the input queue and the graphical console echo can all be checked without
# a human at the window.
#
# Usage: tools/type-test.sh [iso] [out.log] [text-to-type] [banner-to-wait-for]
#
# The banner defaults to the kernel console's own prompt.  Pass a different one when
# something else owns the console -- with `make WITH_SHELL=1` the userspace shell does,
# and waiting for a prompt that is never printed is a timeout that looks like a hang.

set -u

ISO=${1:-build/minx.iso}
OUT=${2:-/tmp/minx-type.log}
TEXT=${3:-help
keys
irqs}
BANNER=${4:-minx console commands}

if [ ! -f "$ISO" ]; then
    echo "type-test: $ISO does not exist" >&2
    exit 1
fi

ACCEL="-accel tcg,thread=multi"
if [ -r /dev/kvm ]; then
    ACCEL="-accel kvm"
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/minx-type.XXXXXX")
MON="$TMP/mon.sock"
SERIAL="$TMP/serial.log"
SHOT="$TMP/screen.ppm"

cleanup() {
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    if [ -n "${SHOW:-}" ]; then
        echo "--- serial ---"
        cat "$SERIAL"
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

qemu-system-x86_64 -m 512M -smp "${SMP:-4}" -no-reboot -boot d \
    -drive "file=$ISO,format=raw,if=none,id=minxcd" \
    -device ide-cd,drive=minxcd \
    -serial "file:$SERIAL" \
    -vga std -display none $ACCEL \
    -monitor "unix:$MON,server,nowait" >/dev/null 2>&1 &
QPID=$!

i=0
while [ ! -S "$MON" ]; do
    i=$((i + 1))
    if [ "$i" -gt 200 ]; then
        echo "type-test: monitor socket never appeared" >&2
        exit 1
    fi
    sleep 0.05
done

# The monitor connection is a plain unix socket, so talk to it with python3
# rather than depending on socat or nc being installed.
python3 - "$MON" "$SERIAL" "$TEXT" "$SHOT" "$BANNER" <<'PYEOF'
import socket
import sys
import time

mon_path, serial_path, text, shot, banner = sys.argv[1:6]

def send(sock, line, settle=0.25):
    sock.sendall((line + "\n").encode())
    time.sleep(settle)

def drain(sock):
    sock.settimeout(0.2)
    out = b""
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            out += chunk
    except socket.timeout:
        pass
    return out.decode(errors="replace")

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(mon_path)
drain(s)

# Wait for the console banner before typing into it.
deadline = time.time() + 40
while time.time() < deadline:
    try:
        with open(serial_path) as fh:
            if banner in fh.read():
                break
    except OSError:
        pass
    time.sleep(0.2)
else:
    print("type-test: the console banner never appeared", file=sys.stderr)
    sys.exit(1)

print("type-test: banner seen, typing")

# `sendkey` takes a QKeyCode plus optional modifiers.  qemu-c64 and
# keya-to-z cover the text; enter is ret, and space is spc.
KEYMAP = {" ": "spc"}
for ch in text:
    if ch == "\n":
        send(s, "sendkey ret")
    elif ch.lower() in KEYMAP:
        send(s, "sendkey " + KEYMAP[ch])
    elif ch.isalnum():
        send(s, "sendkey " + ch.lower())
    else:
        print(f"type-test: no key mapping for {ch!r}", file=sys.stderr)

time.sleep(1.0)

# Ask QEMU what it thinks the interrupt controllers are doing.  The kernel's
# own log cannot see the local APIC at all, because the bootloader's page tables
# do not map MMIO, so this is the only way to see its state.
send(s, "info pic", settle=0.6)
send(s, "info lapic", settle=0.6)
mon_out = drain(s)
print(mon_out)

send(s, f"screendump {shot}", settle=1.5)
send(s, "quit", settle=0.2)
drain(s)
s.close()
PYEOF
PY_STATUS=$?

cp "$SERIAL" "$OUT" 2>/dev/null || true
if [ -f "$SHOT" ]; then
    echo "type-test: screendump written ($SHOT, $(wc -c < "$SHOT") bytes)"
    cp "$SHOT" "$(dirname "$OUT")/minx-type-screen.ppm" 2>/dev/null || true
else
    echo "type-test: no screendump produced" >&2
fi
exit "$PY_STATUS"