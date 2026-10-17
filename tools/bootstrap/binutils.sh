#!/usr/bin/env bash
# Build GNU binutils as an x86_64-elf cross toolchain (ld, objcopy, readelf,
# objdump, nm, ...) into <repo>/.toolchain/cross.
#
# Idempotent: does nothing if the tools are already installed at the pinned
# version. Works on macOS and Linux; installs nothing outside the repo.
#
#   tools/bootstrap/binutils.sh            # build if needed
#   FORCE=1 tools/bootstrap/binutils.sh    # rebuild
#   JOBS=4 tools/bootstrap/binutils.sh     # parallelism (default 8)
set -euo pipefail

BINUTILS_VERSION="2.45.1"
BINUTILS_SHA256="5fe101e6fe9d18fdec95962d81ed670fdee5f37e3f48f0bef87bddf862513aa5"
BINUTILS_TARBALL="binutils-${BINUTILS_VERSION}.tar.xz"
BINUTILS_URLS=(
    "https://ftp.gnu.org/gnu/binutils/${BINUTILS_TARBALL}"
    "https://mirrors.kernel.org/gnu/binutils/${BINUTILS_TARBALL}"
)
TARGET="x86_64-elf"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOLCHAIN="${ROOT}/.toolchain"
PREFIX="${TOOLCHAIN}/cross"
SRC_ROOT="${TOOLCHAIN}/src"
DOWNLOADS="${SRC_ROOT}/downloads"
SRC_DIR="${SRC_ROOT}/binutils-${BINUTILS_VERSION}"
BUILD_DIR="${SRC_ROOT}/build-binutils-${BINUTILS_VERSION}"
STAMP="${PREFIX}/.binutils-${BINUTILS_VERSION}.installed"
JOBS="${JOBS:-8}"

TOOLS=(ld objcopy readelf objdump nm)

log() { printf '[binutils] %s\n' "$*"; }
die() { printf '[binutils] error: %s\n' "$*" >&2; exit 1; }

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        die "need sha256sum or shasum"
    fi
}

print_tools() {
    log "installed tools:"
    for t in "${TOOLS[@]}"; do
        printf '  %s\n' "${PREFIX}/bin/${TARGET}-${t}"
    done
}

already_installed() {
    [[ -f "${STAMP}" ]] || return 1
    for t in "${TOOLS[@]}"; do
        [[ -x "${PREFIX}/bin/${TARGET}-${t}" ]] || return 1
    done
    "${PREFIX}/bin/${TARGET}-ld" --version 2>/dev/null | head -n1 | grep -q "${BINUTILS_VERSION}"
}

if [[ "${FORCE:-0}" != "1" ]] && already_installed; then
    log "binutils ${BINUTILS_VERSION} for ${TARGET} already installed"
    print_tools
    exit 0
fi

command -v make >/dev/null 2>&1 || die "make not found"
command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1 \
    || die "no C compiler found"

mkdir -p "${DOWNLOADS}" "${PREFIX}"

# --- download + verify -------------------------------------------------------
TARBALL_PATH="${DOWNLOADS}/${BINUTILS_TARBALL}"
if [[ -f "${TARBALL_PATH}" ]] && [[ "$(sha256_of "${TARBALL_PATH}")" != "${BINUTILS_SHA256}" ]]; then
    log "cached tarball has wrong checksum; re-downloading"
    rm -f "${TARBALL_PATH}"
fi
if [[ ! -f "${TARBALL_PATH}" ]]; then
    ok=0
    for url in "${BINUTILS_URLS[@]}"; do
        log "downloading ${url}"
        if curl -fL --retry 3 -o "${TARBALL_PATH}.part" "${url}"; then
            ok=1
            break
        fi
    done
    [[ "${ok}" == 1 ]] || die "download failed"
    mv "${TARBALL_PATH}.part" "${TARBALL_PATH}"
fi
actual="$(sha256_of "${TARBALL_PATH}")"
if [[ "${actual}" != "${BINUTILS_SHA256}" ]]; then
    rm -f "${TARBALL_PATH}"
    die "sha256 mismatch for ${BINUTILS_TARBALL}: expected ${BINUTILS_SHA256}, got ${actual}"
fi
log "sha256 verified: ${actual}"

# --- extract -----------------------------------------------------------------
if [[ ! -f "${SRC_DIR}/configure" ]]; then
    log "extracting ${BINUTILS_TARBALL}"
    rm -rf "${SRC_DIR}"
    tar -xJf "${TARBALL_PATH}" -C "${SRC_ROOT}"
    [[ -f "${SRC_DIR}/configure" ]] || die "extraction did not produce ${SRC_DIR}/configure"
fi

# --- configure + build + install --------------------------------------------
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

EXTRA_CONFIGURE=()
if [[ "$(uname -s)" == "Darwin" ]]; then
    # binutils' bundled zlib defines fdopen() as a macro on macOS, which breaks
    # against the current SDK's <stdio.h>. The SDK ships zlib, so use it.
    EXTRA_CONFIGURE+=(--with-system-zlib)
fi

log "configuring (target=${TARGET}, prefix=${PREFIX})"
"${SRC_DIR}/configure" \
    --target="${TARGET}" \
    --prefix="${PREFIX}" \
    --with-sysroot \
    --disable-nls \
    --disable-werror \
    --disable-gdb \
    --disable-gdbserver \
    --disable-sim \
    --disable-gprofng \
    --disable-libdecnumber \
    --disable-readline \
    --disable-multilib \
    --without-zstd \
    --without-debuginfod \
    --without-msgpack \
    ${EXTRA_CONFIGURE[@]+"${EXTRA_CONFIGURE[@]}"} \
    MAKEINFO=true

log "building with -j${JOBS}"
make -j"${JOBS}" MAKEINFO=true
log "installing"
make install MAKEINFO=true

for t in "${TOOLS[@]}"; do
    [[ -x "${PREFIX}/bin/${TARGET}-${t}" ]] || die "missing ${PREFIX}/bin/${TARGET}-${t} after install"
done
touch "${STAMP}"

# The build tree is large and no longer needed.
cd "${ROOT}"
rm -rf "${BUILD_DIR}"

"${PREFIX}/bin/${TARGET}-ld" --version | head -n1
print_tools
