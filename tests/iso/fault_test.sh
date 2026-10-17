#!/usr/bin/env bash
# Bare-metal fault isolation, serial line input and `osef-image run --test`
# exit codes (run by `make test-iso`, which builds build/faultos.{elf,iso}
# from tests/iso/faultos/main.cpp).
#
# Environment (set by the Makefile): OSEF_IMAGE QEMU BUILD PYTHON
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${OSEF_IMAGE:?}" "${QEMU:?}" "${BUILD:?}"
PYTHON="${PYTHON:-python3}"
TIMEOUT="${OSEF_BOOT_TIMEOUT:-90}"
elf="$BUILD/faultos.elf"
iso="$BUILD/faultos.iso"
log="$BUILD/faultos-serial.log"
shot="$BUILD/faultos-screen"

fail=0
check() {  # <description> <command...>
    local what="$1"
    shift
    if "$@"; then echo "  ok   $what"; else echo "  FAIL $what" >&2; fail=$((fail + 1)); fi
}
plain() { sed -e $'s/\x1b\\[[0-9;=?]*[A-Za-z]//g' -e $'s/\r//g' "$1"; }
has() { plain "$1" | grep -qF -- "$2"; }
lacks() { ! plain "$1" | grep -qF -- "$2"; }
line_is() { plain "$1" | grep -qx -- "$2"; }
line_of() { plain "$1" | grep -nF -- "$2" | head -1 | cut -d: -f1 | grep . || echo 0; }
before() { local a b; a=$(line_of "$1" "$2"); b=$(line_of "$1" "$3"); [ "$a" -gt 0 ] && [ "$b" -gt "$a" ]; }

echo "== fault test: FaultOS"
[ -f "$elf" ] && [ -f "$iso" ] || { echo "  FAIL $iso / $elf missing" >&2; exit 1; }

# ---- osef-image run --test: QEMU failures are never reported as exit code 0
"$OSEF_IMAGE" run "$BUILD/no-such-image.iso" --kernel "$elf" --qemu "$QEMU" --test --timeout 30 >/dev/null 2>&1
rc=$?
check "run --test with a missing ISO fails with 126 (got $rc)" [ "$rc" -eq 126 ]
"$OSEF_IMAGE" run "$iso" --kernel "$elf" --qemu "$QEMU" --test --timeout 30 -- -no-such-qemu-option >/dev/null 2>&1
rc=$?
check "run --test when QEMU cannot start fails with 126 (got $rc)" [ "$rc" -eq 126 ]

# ---- boot: every crash kills only its process; escape sequences are dropped
cmds="$BUILD/faultos-boot-commands.txt"
cat >"$cmds" <<'CMDS'
overflow
echo alive after overflow
pf
ud
div
echo alive after faults
@raw echo a\x1b[Db\x1b[A\x1bOHc\x1b[3~d
@screendump
shutdown 0
CMDS
rm -f "$log" "$shot.ppm" "$shot.png"
"$PYTHON" "$HERE/serial_driver.py" --log "$log" --screenshot "$shot.ppm" --timeout "$((TIMEOUT + 10))" \
    --commands "$cmds" -- "$OSEF_IMAGE" run --test --timeout "$TIMEOUT" --kernel "$elf" --qemu "$QEMU" "$iso" \
    -- -qmp "unix:@QMP@,server=on,wait=off"
rc=$?
echo "  serial log: $log"
check "'shutdown 0' ends run --test with exit code 0 (got $rc)" [ "$rc" -eq 0 ]
check "stack overflow is isolated: 'crashed: stack overflow'" has "$log" "(overflow) crashed: stack overflow"
check "the shell is alive after the stack overflow" before "$log" "crashed: stack overflow" "alive after overflow"
check "page fault is isolated" has "$log" "(pf) crashed: page fault at 0x0000000000000010"
check "invalid opcode is isolated" has "$log" "(ud) crashed: invalid opcode"
check "divide error is isolated" has "$log" "(div) crashed: divide error"
check "the shell is alive after the faults" before "$log" "(div) crashed" "alive after faults"
check "terminal escape sequences are dropped from the line ('abcd')" line_is "$log" "abcd"
check "no escape-sequence residue in the line" lacks "$log" "[D"
check "no kernel panic / double fault" lacks "$log" "PANIC"
check "system halted with exit code 0" has "$log" "System halted (exit code 0)"
if [ -f "$shot.ppm" ]; then
    check "framebuffer screenshot $shot.png shows the console" \
        "$PYTHON" "$HERE/ppm2png.py" --require-content "$shot.ppm" "$shot.png"
    rm -f "$shot.ppm"
else
    check "framebuffer screenshot taken" false
fi

if [ "$fail" -eq 0 ]; then echo "  PASS FaultOS"; else echo "fault test: $fail check(s) failed" >&2; exit 1; fi
