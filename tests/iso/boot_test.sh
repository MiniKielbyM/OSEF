#!/usr/bin/env bash
# Boot tests for the example ISOs (run by `make test-iso`, which builds them).
#
#   tests/iso/boot_test.sh <os>...        e.g. minimal myos
#
# For each build/<os>.iso (kernel build/<os>.elf):
#   1. structure: the El Torito catalog has exactly one boot image, for the
#      declared firmware only (platform 0x00 BIOS or 0xEF UEFI);
#   2. boot: headless QEMU with the declared firmware via `osef-image run
#      --test`, shell commands typed on the serial console, output captured in
#      build/<os>-serial.log and checked; a framebuffer screenshot is saved
#      as build/<os>-screen.png; `shutdown 7` must end QEMU with exit code 7;
#   3. cross-check: booting the ISO with the *other* firmware (OVMF for a
#      BIOS ISO, SeaBIOS for a UEFI ISO) must not start the OS
#      (build/<os>-cross-serial.log, build/<os>-cross-screen.png).
#      Set OSEF_CROSS_CHECK=0 to skip it.
#
# Environment (set by the Makefile): OSEF_IMAGE QEMU OVMF XORRISO BUILD PYTHON
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
: "${OSEF_IMAGE:?}" "${QEMU:?}" "${BUILD:?}"
PYTHON="${PYTHON:-python3}"
XORRISO="${XORRISO:-xorriso}"
OVMF="${OVMF:-}"
CROSS="${OSEF_CROSS_CHECK:-1}"
BOOT_TIMEOUT="${OSEF_BOOT_TIMEOUT:-90}"
CROSS_SECONDS="${OSEF_CROSS_SECONDS:-25}"

[ $# -gt 0 ] || { echo "usage: $0 <os>..." >&2; exit 2; }

total_fail=0
fail=0
check() {  # <description> <command...>
    local what="$1"
    shift
    if "$@"; then echo "  ok   $what"; else echo "  FAIL $what" >&2; fail=$((fail + 1)); fi
}
# Serial log without ANSI escapes and CRs, for the substring checks.
plain() { sed -e $'s/\x1b\\[[0-9;=?]*[A-Za-z]//g' -e $'s/\r//g' "$1"; }
has() { plain "$1" | grep -qF -- "$2"; }
lacks() { ! plain "$1" | grep -qF -- "$2"; }
# Line number of the first line containing $2 (0 if none).
line_of() { plain "$1" | grep -nF -- "$2" | head -1 | cut -d: -f1 | grep . || echo 0; }
line_matches() { plain "$1" | grep -q -- "$2"; }
before() { local a b; a=$(line_of "$1" "$2"); b=$(line_of "$1" "$3"); [ "$a" -gt 0 ] && [ "$b" -gt "$a" ]; }

for os in "$@"; do
    fail=0
    elf="$BUILD/$os.elf"
    iso="$BUILD/$os.iso"
    log="$BUILD/$os-serial.log"
    shot="$BUILD/$os-screen"
    echo "== boot test: $os"
    if [ ! -f "$elf" ] || [ ! -f "$iso" ]; then
        echo "  FAIL $iso / $elf missing (run: make iso OS=$os)" >&2
        total_fail=$((total_fail + 1))
        continue
    fi
    info="$("$OSEF_IMAGE" inspect "$elf")" || { total_fail=$((total_fail + 1)); continue; }
    name="$(printf '%s\n' "$info" | sed -n 's/^name=//p')"
    fw="$(printf '%s\n' "$info" | sed -n 's/^firmware=//p')"            # BIOS | UEFI
    fw_lc="$(printf '%s' "$fw" | tr 'A-Z' 'a-z')"                       # bios | uefi
    echo "  $name: declared firmware $fw"

    # ---- 1. ISO structure
    report="$("$XORRISO" -indev "$iso" -report_el_torito plain 2>/dev/null)"
    imgs="$(printf '%s\n' "$report" | grep -c '^El Torito boot img' || true)"
    check "El Torito catalog has exactly one boot image (found $imgs)" [ "$imgs" -eq 1 ]
    check "the boot image is for platform $fw" \
        sh -c "printf '%s\n' \"\$1\" | grep '^El Torito boot img' | grep -qw \"\$2\"" _ "$report" "$fw"
    cat_lba="$(printf '%s\n' "$report" | sed -n 's/^El Torito catalog *: *\([0-9][0-9]*\).*/\1/p')"
    if [ -n "$cat_lba" ]; then
        # Validation entry: byte 0 = 0x01, byte 1 = platform id.
        pid="$(dd if="$iso" bs=2048 skip="$cat_lba" count=1 2>/dev/null | od -An -tx1 -N2 | tr -d ' \n')"
        want=$([ "$fw" = BIOS ] && echo 0100 || echo 01ef)
        check "boot catalog platform id is 0x${pid#01} (want 0x${want#01})" [ "$pid" = "$want" ]
    else
        check "boot catalog found" false
    fi
    if [ "$fw" = BIOS ]; then
        check "BIOS ISO has no EFI boot files" \
            sh -c "! \"\$1\" -indev \"\$2\" -find /EFI 2>/dev/null | grep -q EFI" _ "$XORRISO" "$iso"
    else
        check "UEFI ISO has no BIOS boot files" \
            sh -c "! \"\$1\" -indev \"\$2\" -find /boot/limine 2>/dev/null | grep -q limine-bios" _ "$XORRISO" "$iso"
    fi

    # ---- 2. Boot with the declared firmware
    src="$ROOT/examples/$os/main.cpp"
    cmds="$BUILD/$os-boot-commands.txt"
    has_crash=0
    if [ -f "$src" ] && grep -q '"crash"' "$src"; then has_crash=1; fi
    {
        echo "hw"
        echo "uname"
        echo "mem"
        echo "ps"
        echo "echo ok"
        if [ "$has_crash" = 1 ]; then echo "crash"; echo "echo alive after crash"; fi
        echo "@screendump"
        echo "shutdown 7"
    } >"$cmds"
    run_args=(run --test --timeout "$BOOT_TIMEOUT" --kernel "$elf" --qemu "$QEMU")
    if [ "$fw" = UEFI ]; then run_args+=(--ovmf "$OVMF"); fi
    rm -f "$log" "$shot.ppm" "$shot.png"
    "$PYTHON" "$HERE/serial_driver.py" --log "$log" --screenshot "$shot.ppm" --timeout "$((BOOT_TIMEOUT + 10))" \
        --commands "$cmds" -- "$OSEF_IMAGE" "${run_args[@]}" "$iso" -- -qmp "unix:@QMP@,server=on,wait=off"
    rc=$?
    echo "  serial log: $log"
    check "QEMU exit code is 7 from 'shutdown 7' (got $rc)" [ "$rc" -eq 7 ]
    check "kernel reports 'booted via $fw'" has "$log" "booted via $fw"
    check "no firmware mismatch warning" lacks "$log" "but was booted via"
    check "banner: '$name ... (OSEF engine on x86_64, $fw_lc boot)'" has "$log" "(OSEF engine on x86_64, $fw_lc boot)"
    check "banner names the OS" has "$log" "$name "
    check "hw lists 'firmware:   $fw'" has "$log" "firmware:   $fw"
    check "hw lists the declared devices" has "$log" "devices:    serial"
    check "uname: '$name ... x86_64 $fw_lc'" line_matches "$log" "^$name [^ ]* x86_64 $fw_lc\$"
    check "mem reports the kernel heap" has "$log" "total "
    check "ps lists the shell" has "$log" " sh *"
    check "echo ok printed 'ok'" line_matches "$log" "^ok\$"
    if [ "$has_crash" = 1 ]; then
        check "crash is isolated: 'crashed: segfault (simulated)'" has "$log" "crashed: segfault (simulated)"
        check "the shell is still alive after the crash" before "$log" "crashed: segfault (simulated)" "alive after crash"
    fi
    check "no bootloader or kernel panic" lacks "$log" "PANIC"
    check "system halted with exit code 7" has "$log" "System halted (exit code 7)"
    if [ -f "$shot.ppm" ]; then
        check "framebuffer screenshot $shot.png shows the console" \
            "$PYTHON" "$HERE/ppm2png.py" --require-content "$shot.ppm" "$shot.png"
        rm -f "$shot.ppm"
    else
        check "framebuffer screenshot taken" false
    fi

    # ---- 3. Cross-check: the other firmware must not boot this ISO
    if [ "$CROSS" != 0 ]; then
        xlog="$BUILD/$os-cross-serial.log"
        xshot="$BUILD/$os-cross-screen"
        mem="$(printf '%s\n' "$info" | sed -n 's/^memory_mb=//p')"
        # Same VM size rule as `osef-image run`: UEFI firmware + Limine need >= 128 MiB.
        if [ "$fw" = BIOS ] && [ "$mem" -lt 128 ]; then mem=128; fi
        qemu_args=(-M q35 -m "$mem" -cdrom "$iso" -boot d -serial stdio -display none -no-reboot
                   -device isa-debug-exit,iobase=0xf4,iosize=0x04 -qmp "unix:@QMP@,server=on,wait=off")
        if [ "$fw" = BIOS ]; then
            other="UEFI (OVMF)"
            qemu_args+=(-drive "if=pflash,format=raw,readonly=on,file=$OVMF")
        else
            other="BIOS (SeaBIOS)"
        fi
        if [ "$fw" = BIOS ] && [ -z "$OVMF" ]; then
            echo "  skip cross-check: no OVMF firmware"
        else
            echo "  cross-check: booting the $fw-only ISO with $other for ${CROSS_SECONDS}s"
            rm -f "$xlog" "$xshot.ppm" "$xshot.png"
            "$PYTHON" "$HERE/serial_driver.py" --log "$xlog" --screenshot "$xshot.ppm" --observe "$CROSS_SECONDS" \
                -- "$QEMU" "${qemu_args[@]}" 2>/dev/null
            xrc=$?
            check "$other does not start the OS (no kernel output)" lacks "$xlog" "OSEF bare-metal x86_64 kernel"
            check "$other does not report 'booted via'" lacks "$xlog" "booted via"
            check "$other leaves the VM running without an OSEF exit (driver status $xrc)" [ "$xrc" -eq 124 ]
            if [ -f "$xshot.ppm" ]; then
                "$PYTHON" "$HERE/ppm2png.py" "$xshot.ppm" "$xshot.png" >/dev/null && rm -f "$xshot.ppm"
                echo "  cross-check screenshot: $xshot.png, serial: $xlog"
            fi
        fi
    fi

    if [ "$fail" -eq 0 ]; then echo "  PASS $os ($fw)"; else echo "  $os: $fail check(s) failed" >&2; fi
    total_fail=$((total_fail + fail))
done

if [ "$total_fail" -eq 0 ]; then
    echo "test-iso: all boot tests passed"
else
    echo "test-iso: $total_fail check(s) failed" >&2
    exit 1
fi
