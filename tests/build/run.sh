#!/usr/bin/env bash
# Build-system regression tests for the root Makefile, driven exactly like the
# editor drives it (`make <goal> OS_DIR=<dir>`), on scratch OS directories.
#
#   tests/build/run.sh hosted     `make os` / `make run`   (part of `make test`)
#   tests/build/run.sh iso        `make kernel` / `make iso` (part of `make test-iso`)
#
# Covered:
#   * an edit that make's mtime check misses (same second as the last build,
#     simulated by back-dating the edited file) is still rebuilt;
#   * a deleted source file is dropped from the next link;
#   * OS_DIR may contain spaces;
#   * standard-library use: check-source warns; the ISO build fails with an
#     explanation (iso);
#   * `make iso` exports <project>/<os>.iso and says "ISO written" when it built the ISO, "ISO up to date"
#     when nothing changed (iso).
#
# Environment (set by the Makefile): MAKE ROOT OUT [SANITIZE]
set -u
: "${ROOT:?ROOT not set}" "${OUT:?OUT not set}"
MAKE="${MAKE:-make}"
MODE="${1:-hosted}"
case "$MODE" in hosted | iso) ;; *) echo "usage: $0 hosted|iso" >&2; exit 2 ;; esac

passed=0
failed=0
ok() { passed=$((passed + 1)); echo "  ok   $1"; }
bad() { failed=$((failed + 1)); echo "  FAIL $1" >&2; if [ -n "${2:-}" ] && [ -f "$2" ]; then sed 's/^/       | /' "$2" | tail -25 >&2; fi; }

# m <log> <make args...>: run the root Makefile quietly, output in <log>.
m() {
    local log="$1"
    shift
    "$MAKE" --no-print-directory -C "$ROOT" "$@" >"$log" 2>&1
}

# Freshly written scratch OS: main.cpp (+ greet.h, + extra.cpp).
new_os() {  # <dir> <firmware>
    rm -rf "$1"
    mkdir -p "$1"
    cat >"$1/main.cpp" <<EOF
#include <osef/hardware.h>

OSEF_HARDWARE(osef::Hardware()
    .name("BuildTestOS")
    .arch(osef::Arch::x86_64)
    .firmware(osef::Firmware::$2)
    .devices(osef::Hw::Serial)
    .memory_mb(64));

#include <osef/osef.h>

#include "greet.h"

int main() {
    osef::Kernel kernel;
    kernel.add_module<osef::ShellModule>();
    kernel.programs().add_fn("greet", "prints the greeting", [](osef::Context& c) {
        c.println(kGreeting);
        c.exit(0);
    });
    return kernel.run();
}
EOF
    greeting "$1" v1
    cat >"$1/extra.cpp" <<'EOF'
// Linked in as long as this file exists; its marker shows up in the binary.
extern "C" __attribute__((used)) const char osef_build_test_extra[] = "osef-build-test-extra-marker";
EOF
}

greeting() { printf 'static const char* const kGreeting = "osef-greeting-%s";\n' "$2" >"$1/greet.h"; }

# Pretend the file was saved in the same second as (or before) the last
# build: make 3.81 compares whole seconds, so it would not see the edit.
backdate() { touch -t 200001010000 "$@"; }

contains() { grep -aq -- "$2" "$1" 2>/dev/null; }

echo "== build-system checks ($MODE)"
mkdir -p "$OUT"

if [ "$MODE" = hosted ]; then
    dir="$OUT/hosted-os"
    new_os "$dir" BIOS
    bin() { find "$ROOT/build/os" -path "*_hosted-os/hosted*/hosted-os" -type f 2>/dev/null | head -1; }
    if m "$OUT/h1.log" os OS_DIR="$dir" && contains "$(bin)" osef-greeting-v1; then
        ok "make os OS_DIR=<dir> builds the OS"
    else
        bad "make os OS_DIR=<dir> failed" "$OUT/h1.log"
    fi

    greeting "$dir" v2
    backdate "$dir/greet.h" "$dir/main.cpp"
    if m "$OUT/h2.log" os OS_DIR="$dir" && contains "$(bin)" osef-greeting-v2; then
        ok "an edit with an old mtime (same-second save) is rebuilt"
    else
        bad "the hosted binary still has the old greeting after an edit with an old mtime" "$OUT/h2.log"
    fi

    contains "$(bin)" osef-build-test-extra-marker && ok "extra.cpp is linked" || bad "extra.cpp is not linked"
    rm -f "$dir/extra.cpp"
    if m "$OUT/h3.log" os OS_DIR="$dir" && ! contains "$(bin)" osef-build-test-extra-marker; then
        ok "a deleted source is dropped from the next link"
    else
        bad "the hosted binary still contains the deleted extra.cpp" "$OUT/h3.log"
    fi

    if m "$OUT/h4.log" os OS_DIR="$dir" && ! grep -q "c++\|clang" "$OUT/h4.log"; then
        ok "nothing is rebuilt when nothing changed"
    else
        bad "a second build without changes recompiled something" "$OUT/h4.log"
    fi

    sdir="$OUT/my hosted os"
    new_os "$sdir" UEFI
    if printf 'greet\nexit\n' | m "$OUT/h5.log" run OS_DIR="$sdir" && grep -q "osef-greeting-v1" "$OUT/h5.log"; then
        ok "make run OS_DIR=<dir with spaces> builds and runs the OS"
    else
        bad "make run OS_DIR=<dir with spaces> failed" "$OUT/h5.log"
    fi

    stddir="$OUT/hosted-std"
    new_os "$stddir" BIOS
    printf '#include <string>\nstd::string osef_build_test_std() { return "x"; }\n' >"$stddir/extra.cpp"
    m "$OUT/h6.log" os OS_DIR="$stddir"
    if grep -q "extra.cpp:2: warning: std::string is not available in the ISO build" "$OUT/h6.log" &&
        grep -q "extra.cpp:1: warning: #include <string> is not available in the ISO build" "$OUT/h6.log"; then
        ok "check-source warns about standard-library use in a hosted build"
    else
        bad "no warning about <string> / std::string in the hosted build" "$OUT/h6.log"
    fi
else
    dir="$OUT/iso-os"
    new_os "$dir" UEFI
    elf="$ROOT/build/iso-os.elf"
    if m "$OUT/i1.log" iso OS_DIR="$dir" && grep -q "^ISO written: os=BuildTestOS firmware=UEFI" "$OUT/i1.log" &&
        contains "$elf" osef-greeting-v1; then
        ok "make iso OS_DIR=<dir> prints 'ISO written: ...'"
    else
        bad "make iso OS_DIR=<dir> failed" "$OUT/i1.log"
    fi
    if m "$OUT/i2.log" iso OS_DIR="$dir" && grep -q "^ISO up to date: os=BuildTestOS firmware=UEFI path=$dir/iso-os.iso" "$OUT/i2.log" && [ -f "$ROOT/build/iso-os.iso" ] &&
        ! grep -q "^ISO written" "$OUT/i2.log"; then
        ok "a second make iso prints 'ISO up to date: ...'"
    else
        bad "a second make iso without changes did not say 'ISO up to date'" "$OUT/i2.log"
    fi

    greeting "$dir" v2
    backdate "$dir/greet.h" "$dir/main.cpp"
    if m "$OUT/i3.log" iso OS_DIR="$dir" && contains "$elf" osef-greeting-v2 && grep -q "^ISO written" "$OUT/i3.log"; then
        ok "an edit with an old mtime (same-second save) is rebuilt into the kernel and ISO"
    else
        bad "the kernel still has the old greeting after an edit with an old mtime" "$OUT/i3.log"
    fi

    contains "$elf" osef-build-test-extra-marker && ok "extra.cpp is linked into the kernel" || bad "extra.cpp is not in the kernel"
    rm -f "$dir/extra.cpp"
    if m "$OUT/i4.log" kernel OS_DIR="$dir" && ! contains "$elf" osef-build-test-extra-marker; then
        ok "a deleted source is dropped from the next kernel link"
    else
        bad "the kernel still contains the deleted extra.cpp" "$OUT/i4.log"
    fi

    sdir="$OUT/my iso os"
    new_os "$sdir" BIOS
    if m "$OUT/i5.log" iso OS_DIR="$sdir" &&
        grep -q "^ISO written: os=BuildTestOS firmware=BIOS path=$sdir/my_iso_os.iso (source: $sdir)" "$OUT/i5.log" &&
        [ -f "$sdir/my_iso_os.iso" ] && [ -f "$ROOT/build/my_iso_os.iso" ]; then
        ok "make iso OS_DIR=<dir with spaces> exports <dir>/my_iso_os.iso (and build/my_iso_os.iso)"
    else
        bad "make iso OS_DIR=<dir with spaces> failed" "$OUT/i5.log"
    fi

    stddir="$OUT/iso-std"
    new_os "$stddir" BIOS
    printf '#include <string>\nstd::string osef_build_test_std() { return "x"; }\n' >"$stddir/extra.cpp"
    if m "$OUT/i6.log" iso OS_DIR="$stddir"; then
        bad "an OS using <string> built an ISO" "$OUT/i6.log"
    elif grep -q "'string' file not found" "$OUT/i6.log" &&
        grep -q "the ISO kernel is freestanding: it has no C/C++ standard library" "$OUT/i6.log"; then
        ok "an OS using <string> fails the ISO build with an explanation"
    else
        bad "the ISO build of an OS using <string> failed without the explanation" "$OUT/i6.log"
    fi
    rm -f "$ROOT/build/iso-os.elf" "$ROOT/build/iso-os.iso" "$ROOT/build/my_iso_os.elf" "$ROOT/build/my_iso_os.iso"
fi

echo "build-system checks ($MODE): $passed passed, $failed failed"
[ "$failed" -eq 0 ]
