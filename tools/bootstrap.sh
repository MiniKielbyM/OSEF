#!/usr/bin/env bash
# OSEF bootstrap: get every tool the ISO pipeline needs, building only what is missing.
#
#   tools/bootstrap.sh                 linker, xorriso, Limine, CMake (whatever is missing)
#   tools/bootstrap.sh --with-qemu     also QEMU + UEFI firmware (for run-iso / test-iso)
#   tools/bootstrap.sh --with-editor   also the OSEF editor's third-party sources
#   tools/bootstrap.sh --check         only report what is there; build nothing
#   tools/bootstrap.sh --rerun         run the selected per-tool scripts even when the tool
#                                      is already there (each script still skips up-to-date work)
#
# Each tool comes from the system when it is already installed (ld.lld or
# x86_64-elf-ld, xorriso, cmake >= 3.21, qemu-system-x86_64 + OVMF), from an
# earlier bootstrap in .toolchain/, or else from the matching script in
# tools/bootstrap/ (binutils.sh, xorriso.sh, limine.sh, cmake.sh, qemu.sh,
# editor-deps.sh), which installs into .toolchain/ (editor deps into
# editor/third_party/). Nothing is installed outside the repository.
#
# clang cannot be bootstrapped: install clang/LLVM (Xcode command line tools on
# macOS, `apt install clang lld` on Debian/Ubuntu, the LLVM installer on Windows).
#
# Prints a summary table at the end. Exit status: 0 when every selected tool is
# available, 1 otherwise.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TC="${ROOT}/.toolchain"
BOOT="${SCRIPT_DIR}/bootstrap"

WITH_QEMU=0
WITH_EDITOR=0
CHECK_ONLY=0
RERUN=0
usage() { sed -n '2,24p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
for arg in "$@"; do
    case "$arg" in
        --with-qemu) WITH_QEMU=1 ;;
        --with-editor) WITH_EDITOR=1 ;;
        --check) CHECK_ONLY=1 ;;
        --rerun) RERUN=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "bootstrap: unknown option: $arg (see --help)" >&2; exit 2 ;;
    esac
done

if [ -t 1 ]; then log() { printf '\033[1m[bootstrap]\033[0m %s\n' "$*"; }; else log() { printf '[bootstrap] %s\n' "$*"; }; fi
have() { command -v "$1" 2>/dev/null; }

ROWS=()     # "tool|status|path" for the summary
FAILED=0
row() { ROWS+=("$1|$2|$3"); }

# run_script <script> : runs tools/bootstrap/<script>; returns its exit status.
run_script() {
    log "running tools/bootstrap/$1"
    bash "${BOOT}/$1"
}

# ---------------------------------------------------------------- lookups
# Each prints the path of a usable tool (empty if none), in the same order the
# Makefile and CMake look: .toolchain first, then the system.
find_clang() {
    local c
    for c in clang++ clang++-21 clang++-20 clang++-19 clang++-18 clang++-17 clang++-16 clang++-15; do
        c="$(have "$c")" || continue
        if printf 'extern "C" int osef_probe(int x) { return x * 3; }\n' \
            | "$c" -std=c++20 --target=x86_64-unknown-none-elf -ffreestanding -x c++ -c - -o /dev/null 2>/dev/null; then
            echo "$c"
            return
        fi
    done
}
find_linker() {
    if [ -x "${TC}/cross/bin/x86_64-elf-ld" ]; then echo "${TC}/cross/bin/x86_64-elf-ld"; return; fi
    have ld.lld || have x86_64-elf-ld || true
}
find_xorriso() {
    if [ -x "${TC}/xorriso/bin/xorriso" ]; then echo "${TC}/xorriso/bin/xorriso"; return; fi
    have xorriso || true
}
find_limine() {
    local d="${TC}/limine" f
    for f in limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin BOOTX64.EFI limine; do
        [ -e "$d/$f" ] || return 0
    done
    [ -f "${TC}/limine-protocol/include/limine.h" ] || return 0
    echo "$d"
}
cmake_ok() {  # <cmake> : version >= 3.21?
    local v
    v="$("$1" --version 2>/dev/null | sed -n 's/^cmake version \([0-9]*\)\.\([0-9]*\).*/\1 \2/p')"
    [ -n "$v" ] || return 1
    set -- $v
    [ "$1" -gt 3 ] || { [ "$1" -eq 3 ] && [ "$2" -ge 21 ]; }
}
find_cmake() {
    if [ -x "${TC}/cmake/bin/cmake" ] && cmake_ok "${TC}/cmake/bin/cmake"; then echo "${TC}/cmake/bin/cmake"; return; fi
    local c
    c="$(have cmake)" && cmake_ok "$c" && echo "$c"
    true
}
find_qemu() {
    if [ -x "${TC}/qemu/bin/qemu-system-x86_64" ]; then echo "${TC}/qemu/bin/qemu-system-x86_64"; return; fi
    have qemu-system-x86_64 || true
}
find_ovmf() {
    local f
    for f in "${TC}/qemu/share/qemu/edk2-x86_64-code.fd" \
             /opt/homebrew/share/qemu/edk2-x86_64-code.fd /usr/local/share/qemu/edk2-x86_64-code.fd \
             /usr/share/qemu/edk2-x86_64-code.fd /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
             /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/edk2/ovmf/OVMF_CODE.fd; do
        if [ -f "$f" ]; then echo "$f"; return; fi
    done
}
source_of() {  # <path> : where a tool comes from
    case "$1" in
        "${TC}"/*) echo ".toolchain" ;;
        "${ROOT}"/*) echo "repo" ;;
        *) echo "system" ;;
    esac
}

# ensure <label> <finder> <script> : use what is there, else run the script.
ensure() {
    local label="$1" finder="$2" script="$3" path status
    path="$($finder)"
    if [ -n "$path" ] && [ "$RERUN" = 0 ]; then
        row "$label" "$(source_of "$path")" "$path"
        return
    fi
    if [ "$CHECK_ONLY" = 1 ]; then
        if [ -n "$path" ]; then row "$label" "$(source_of "$path")" "$path"
        else row "$label" "MISSING" "run: tools/bootstrap/$script"; FAILED=1; fi
        return
    fi
    if run_script "$script"; then
        path="$($finder)"
        if [ -n "$path" ]; then
            status="$(source_of "$path")"
            [ "$status" = ".toolchain" ] && status="built (.toolchain)"
            row "$label" "$status" "$path"
            return
        fi
    fi
    row "$label" "FAILED" "tools/bootstrap/$script (see the output above)"
    FAILED=1
}

# ---------------------------------------------------------------- tools
mkdir -p "$TC"

clang="$(find_clang)"
if [ -n "$clang" ]; then
    row "clang++" "system" "$clang"
else
    row "clang++" "MISSING" "install clang/LLVM (cannot be bootstrapped)"
    FAILED=1
fi

ensure "ELF linker" find_linker binutils.sh
ensure "xorriso" find_xorriso xorriso.sh
ensure "Limine" find_limine limine.sh
ensure "CMake" find_cmake cmake.sh

if [ "$WITH_QEMU" = 1 ]; then
    qemu="$(find_qemu)"
    ovmf="$(find_ovmf)"
    if [ -n "$qemu" ] && [ -n "$ovmf" ] && [ "$RERUN" = 0 ]; then
        row "QEMU" "$(source_of "$qemu")" "$qemu"
        row "OVMF (UEFI)" "$(source_of "$ovmf")" "$ovmf"
    elif [ "$CHECK_ONLY" = 1 ]; then
        row "QEMU" "${qemu:+$(source_of "$qemu")}${qemu:-MISSING}" "${qemu:-run: tools/bootstrap/qemu.sh}"
        row "OVMF (UEFI)" "${ovmf:+$(source_of "$ovmf")}${ovmf:-MISSING}" "${ovmf:-run: tools/bootstrap/qemu.sh}"
        { [ -n "$qemu" ] && [ -n "$ovmf" ]; } || FAILED=1
    elif [ "$(uname -s)" != Darwin ]; then
        # tools/bootstrap/qemu.sh builds a macOS QEMU; elsewhere use the package manager.
        row "QEMU" "${qemu:+$(source_of "$qemu")}${qemu:-MISSING}" "${qemu:-install qemu-system-x86 (apt) / qemu (dnf, pacman)}"
        row "OVMF (UEFI)" "${ovmf:+$(source_of "$ovmf")}${ovmf:-MISSING}" "${ovmf:-install ovmf (apt) / edk2-ovmf (dnf, pacman)}"
        { [ -n "$qemu" ] && [ -n "$ovmf" ]; } || FAILED=1
    else
        if run_script qemu.sh; then
            qemu="$(find_qemu)"
            ovmf="$(find_ovmf)"
        fi
        if [ -n "$qemu" ] && [ -n "$ovmf" ]; then
            row "QEMU" "built (.toolchain)" "$qemu"
            row "OVMF (UEFI)" "$(source_of "$ovmf")" "$ovmf"
        else
            row "QEMU" "FAILED" "tools/bootstrap/qemu.sh (see the output above)"
            FAILED=1
        fi
    fi
else
    qemu="$(find_qemu)"
    row "QEMU" "${qemu:+$(source_of "$qemu")}${qemu:-skipped}" "${qemu:-pass --with-qemu to build it}"
fi

editor_deps_present() {
    local d
    for d in imgui glfw fonts; do
        [ -f "${ROOT}/editor/third_party/$d/.osef-version" ] || return 0
    done
    echo "${ROOT}/editor/third_party"
}
if [ "$WITH_EDITOR" = 1 ]; then
    ensure "editor deps" editor_deps_present editor-deps.sh
else
    deps="$(editor_deps_present)"
    row "editor deps" "${deps:+present}${deps:-skipped}" "${deps:-pass --with-editor to fetch them}"
fi

# ---------------------------------------------------------------- summary
echo
printf '%-13s %-20s %s\n' "TOOL" "STATUS" "PATH"
printf '%-13s %-20s %s\n' "-------------" "--------------------" "----"
for r in "${ROWS[@]}"; do
    IFS='|' read -r tool status path <<<"$r"
    printf '%-13s %-20s %s\n' "$tool" "$status" "$path"
done
echo
if [ "$FAILED" = 0 ]; then
    echo "bootstrap: ready. Next: make iso OS=minimal   (or: cmake -S . -B build && cmake --build build)"
else
    echo "bootstrap: some tools are missing (see above)." >&2
fi
exit "$FAILED"
