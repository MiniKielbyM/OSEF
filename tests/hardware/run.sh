#!/usr/bin/env bash
# OSEF_HARDWARE checks (run by `make hardware-check`, part of `make test`).
#
# Every fixture in this directory is a complete OS main.cpp. Bad declarations
# must be rejected (by `osef-image check-source` or by the compiler, with a
# readable message); good ones must compile freestanding and carry the
# declared hardware in the object's .osef_hardware section.
#
# Environment (set by the Makefile):
#   OSEF_IMAGE   host tool          BM_CXX / BM_CXXFLAGS   freestanding compiler + flags
#   OUT          scratch directory  HOST_CXX / HOST_CXXFLAGS  hosted compiler + flags (optional)
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${OSEF_IMAGE:?OSEF_IMAGE not set}" "${BM_CXX:?BM_CXX not set}" "${BM_CXXFLAGS:?BM_CXXFLAGS not set}" "${OUT:?OUT not set}"
mkdir -p "$OUT"
if ! command -v "$BM_CXX" >/dev/null 2>&1; then
    echo "hardware-check: error: $BM_CXX not found. These checks compile freestanding x86_64 ELF objects" >&2
    echo "  and need clang (pass BM_CXX=<clang++>)." >&2
    exit 1
fi

passed=0
failed=0
ok() { passed=$((passed + 1)); echo "  ok   $1"; }
bad() { failed=$((failed + 1)); echo "  FAIL $1" >&2; if [ -n "${2:-}" ] && [ -f "$2" ]; then sed 's/^/       | /' "$2" | head -20 >&2; fi; }

# compile <fixture> -> exit status; diagnostics in $OUT/<fixture>.log
compile() {
    # shellcheck disable=SC2086  # BM_CXXFLAGS is a flag list
    "$BM_CXX" $BM_CXXFLAGS -c "$HERE/$1.cpp" -o "$OUT/$1.o" >"$OUT/$1.log" 2>&1
}

expect_compile_error() {  # <fixture> <text the diagnostics must contain>
    local f="$1" text="$2"
    rm -f "$OUT/$f.o"
    if compile "$f"; then
        bad "$f.cpp compiled, but must be rejected" "$OUT/$f.log"
    elif grep -qF -- "$text" "$OUT/$f.log"; then
        ok "$f.cpp rejected by the compiler: $text"
    else
        bad "$f.cpp failed to compile, but without '$text'" "$OUT/$f.log"
    fi
}

expect_valid() {  # <fixture> <key=value>...
    local f="$1"
    shift
    if ! "$OSEF_IMAGE" check-source "$HERE/$f.cpp" >"$OUT/$f.check.log" 2>&1; then
        bad "$f.cpp: check-source rejected a valid declaration" "$OUT/$f.check.log"
        return
    fi
    ok "$f.cpp passes check-source"
    if ! compile "$f"; then
        bad "$f.cpp does not compile freestanding" "$OUT/$f.log"
        return
    fi
    if [ -s "$OUT/$f.log" ]; then
        bad "$f.cpp compiled with warnings" "$OUT/$f.log"
        return
    fi
    ok "$f.cpp compiles freestanding without warnings"
    if ! "$OSEF_IMAGE" inspect "$OUT/$f.o" >"$OUT/$f.inspect" 2>&1; then
        bad "$f: osef-image inspect failed" "$OUT/$f.inspect"
        return
    fi
    local kv
    for kv in "$@"; do
        if grep -qxF -- "$kv" "$OUT/$f.inspect"; then
            ok "$f: inspect shows $kv"
        else
            bad "$f: inspect does not show $kv" "$OUT/$f.inspect"
        fi
    done
}

echo "== hardware declaration checks (tests/hardware)"

# 1. No declaration: check-source refuses it before anything is compiled.
if "$OSEF_IMAGE" check-source "$HERE/no_declaration.cpp" >"$OUT/no_declaration.check.log" 2>&1; then
    bad "no_declaration.cpp passed check-source" "$OUT/no_declaration.check.log"
elif grep -qF "no hardware declaration found" "$OUT/no_declaration.check.log"; then
    ok "no_declaration.cpp rejected by check-source"
else
    bad "no_declaration.cpp rejected without the expected message" "$OUT/no_declaration.check.log"
fi
# ...and even if compiled anyway, its object carries no declaration.
if compile no_declaration; then
    if "$OSEF_IMAGE" inspect "$OUT/no_declaration.o" >"$OUT/no_declaration.inspect" 2>&1; then
        bad "no_declaration.o has a hardware declaration" "$OUT/no_declaration.inspect"
    elif grep -qF "has no .osef_hardware section" "$OUT/no_declaration.inspect"; then
        ok "no_declaration.o has no .osef_hardware section (inspect refuses it)"
    else
        bad "inspect rejected no_declaration.o for an unexpected reason" "$OUT/no_declaration.inspect"
    fi
else
    bad "no_declaration.cpp should still compile (the link step is what fails)" "$OUT/no_declaration.log"
fi

# 2. Invalid declarations: compile errors that say what to fix.
expect_compile_error no_firmware "OSEF_HARDWARE: choose the boot firmware: .firmware(osef::Firmware::BIOS) or .firmware(osef::Firmware::UEFI)"
expect_compile_error bad_device_flag "OSEF_HARDWARE_unknown_device_flag"
expect_compile_error no_console "OSEF_HARDWARE: enable at least one console device"

# 3. Valid declarations: the firmware (and the rest) reach the kernel image.
expect_valid bios firmware=BIOS name=FixtureBIOS arch=x86_64 devices=serial,ps2-keyboard memory_mb=48 serial_baud=38400
expect_valid uefi firmware=UEFI name=FixtureUEFI arch=x86_64 devices=serial,framebuffer memory_mb=96 \
    resolution=1024x768 boot_timeout=2

# 4. check-source details.
# A too-long .name(): one clear line instead of the compiler's constexpr cascade.
if "$OSEF_IMAGE" check-source "$HERE/long_name.cpp" >"$OUT/long_name.check.log" 2>&1; then
    bad "long_name.cpp passed check-source" "$OUT/long_name.check.log"
elif grep -qF 'long_name.cpp:8: error: OSEF_HARDWARE: .name("NewOS The Most Amazing Operating System Ever") is 44 characters long; the maximum is 31' \
        "$OUT/long_name.check.log" && [ "$(wc -l <"$OUT/long_name.check.log")" -eq 1 ]; then
    ok "long_name.cpp rejected by check-source in one line (44 characters, maximum 31)"
else
    bad "long_name.cpp rejected without the one-line length message" "$OUT/long_name.check.log"
fi
expect_compile_error long_name "OSEF_HARDWARE_name_is_too_long_max_31_characters"
# Digit separators, raw strings and literals mentioning OSEF_HARDWARE( are no
# obstacle; a 31-character name is accepted.
expect_valid literals firmware=UEFI name=LiteralsOS-abcdefghijklmnopqrst
if grep -q "literals.cpp:15: hardware declared" "$OUT/literals.check.log"; then
    ok "literals.cpp: check-source found the declaration on line 15"
else
    bad "literals.cpp: check-source reported the wrong declaration" "$OUT/literals.check.log"
fi
# Standard-library use outside #if OSEF_HOSTED: warned about early, fails freestanding.
"$OSEF_IMAGE" check-source "$HERE/uses_std.cpp" >"$OUT/uses_std.check.log" 2>&1
if grep -q "uses_std.cpp:16: warning: #include <string> is not available in the ISO build" "$OUT/uses_std.check.log" &&
    grep -q "uses_std.cpp:24: warning: std::string is not available in the ISO build" "$OUT/uses_std.check.log" &&
    ! grep -q "uses_std.cpp:19\|uses_std.cpp:20" "$OUT/uses_std.check.log"; then
    ok "uses_std.cpp: check-source warns about <string> / std::string, not about #if OSEF_HOSTED code"
else
    bad "uses_std.cpp: wrong check-source warnings" "$OUT/uses_std.check.log"
fi
expect_compile_error uses_std "'string' file not found"
if "$OSEF_IMAGE" check-source "$HERE/bios.cpp" 2>&1 | grep -q warning; then
    bad "bios.cpp: check-source warns about a clean OS"
else
    ok "bios.cpp: no check-source warnings"
fi

# 5. The public headers do not leak the standard library into hosted OS code.
if [ -n "${HOST_CXX:-}" ]; then
    # shellcheck disable=SC2086
    if $HOST_CXX ${HOST_CXXFLAGS:-} -fsyntax-only -DOSEF_LEAK_CHECK=0 "$HERE/no_std_leak.cpp" >"$OUT/no_std_leak.0.log" 2>&1; then
        ok "no_std_leak.cpp baseline compiles hosted"
    else
        bad "no_std_leak.cpp baseline does not compile hosted" "$OUT/no_std_leak.0.log"
    fi
    for n in 1 2 3 4; do
        what=$(sed -n "/OSEF_LEAK_CHECK == $n\$/{n;p;}" "$HERE/no_std_leak.cpp" | sed 's/[{].*//')
        # shellcheck disable=SC2086
        if $HOST_CXX ${HOST_CXXFLAGS:-} -fsyntax-only -DOSEF_LEAK_CHECK=$n "$HERE/no_std_leak.cpp" >"$OUT/no_std_leak.$n.log" 2>&1; then
            bad "hosted <osef/osef.h> leaks the standard library: '$what' compiles" "$OUT/no_std_leak.$n.log"
        else
            ok "hosted <osef/osef.h> does not leak: '$what' is an error"
        fi
    done
fi

echo "hardware-check: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
