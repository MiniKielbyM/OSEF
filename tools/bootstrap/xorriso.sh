#!/usr/bin/env bash
# Build GNU xorriso (standalone tarball: bundles libburn, libisofs, libisoburn)
# into <repo>/.toolchain/xorriso. Idempotent; installs nothing outside the repo.
#
# Usage: tools/bootstrap/xorriso.sh [--force]
set -euo pipefail

XORRISO_VERSION="1.5.8.pl02"
XORRISO_TARBALL="xorriso-${XORRISO_VERSION}.tar.gz"
XORRISO_SHA256="b1455ecafbf0692ddafe1d71002a96f2ce2d77f4deae602678261ce033f97bc8"
XORRISO_URLS=(
    "https://ftp.gnu.org/gnu/xorriso/${XORRISO_TARBALL}"
    "https://mirrors.kernel.org/gnu/xorriso/${XORRISO_TARBALL}"
)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOLCHAIN="${REPO_ROOT}/.toolchain"
PREFIX="${TOOLCHAIN}/xorriso"
WORK="${TOOLCHAIN}/build/xorriso"
DL="${TOOLCHAIN}/downloads"
STAMP="${PREFIX}/.osef-xorriso-${XORRISO_VERSION}"

log() { printf '[xorriso] %s\n' "$*" >&2; }
die() { printf '[xorriso] error: %s\n' "$*" >&2; exit 1; }

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

jobs() {
    getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2
}

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ "$FORCE" = 0 ] && [ -f "$STAMP" ] && [ -x "${PREFIX}/bin/xorriso" ] \
    && "${PREFIX}/bin/xorriso" -version >/dev/null 2>&1; then
    log "already built (${XORRISO_VERSION})"
    echo "${PREFIX}/bin/xorriso"
    exit 0
fi

command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || die "no C compiler (cc) found"
command -v make >/dev/null 2>&1 || die "make not found"
command -v curl >/dev/null 2>&1 || die "curl not found"

mkdir -p "$DL"
if [ ! -f "${DL}/${XORRISO_TARBALL}" ] || [ "$(sha256_of "${DL}/${XORRISO_TARBALL}")" != "$XORRISO_SHA256" ]; then
    rm -f "${DL}/${XORRISO_TARBALL}"
    ok=0
    for url in "${XORRISO_URLS[@]}"; do
        log "downloading ${url}"
        if curl -fsSL --retry 3 -o "${DL}/${XORRISO_TARBALL}.part" "$url"; then
            mv "${DL}/${XORRISO_TARBALL}.part" "${DL}/${XORRISO_TARBALL}"
            ok=1
            break
        fi
    done
    [ "$ok" = 1 ] || die "download failed"
fi
got="$(sha256_of "${DL}/${XORRISO_TARBALL}")"
[ "$got" = "$XORRISO_SHA256" ] || die "sha256 mismatch for ${XORRISO_TARBALL}: got ${got}, want ${XORRISO_SHA256}"

rm -rf "$WORK"
mkdir -p "$WORK"
tar -xzf "${DL}/${XORRISO_TARBALL}" -C "$WORK"
SRC="$(find "$WORK" -mindepth 1 -maxdepth 1 -type d -name 'xorriso-*' | head -n 1)"
[ -n "$SRC" ] || die "unexpected tarball layout"

log "configuring (prefix ${PREFIX})"
rm -rf "$PREFIX"
(
    cd "$SRC"
    # Optional dependencies are disabled so the build is reproducible and does
    # not pick up random host libraries (or fail on macOS without them).
    ./configure --prefix="$PREFIX" \
        --disable-shared --enable-static \
        --disable-libreadline --disable-libedit \
        --disable-zlib --disable-libbz2 \
        --disable-libacl --disable-xattr \
        --disable-libcdio --disable-lfa-flags --disable-projid \
        --disable-launch-frontend --disable-launch-frontend-setuid \
        --disable-dependency-tracking \
        >"${WORK}/configure.log" 2>&1 || { tail -n 40 "${WORK}/configure.log" >&2; exit 1; }
    log "building"
    make -j"$(jobs)" >"${WORK}/make.log" 2>&1 || { tail -n 60 "${WORK}/make.log" >&2; exit 1; }
    make install >"${WORK}/install.log" 2>&1 || { tail -n 40 "${WORK}/install.log" >&2; exit 1; }
) || die "build failed (logs in ${WORK})"

"${PREFIX}/bin/xorriso" -version >/dev/null 2>&1 || die "built xorriso does not run"
touch "$STAMP"
rm -rf "$WORK"
log "installed: $("${PREFIX}/bin/xorriso" -version 2>/dev/null | head -n 1)"
echo "${PREFIX}/bin/xorriso"
