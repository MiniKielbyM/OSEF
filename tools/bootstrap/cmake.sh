#!/usr/bin/env bash
# Download the official prebuilt CMake into <repo>/.toolchain/cmake and expose
# stable entry points .toolchain/cmake/bin/{cmake,ctest,cpack}.
# Idempotent; installs nothing outside the repo.
#
# Usage: tools/bootstrap/cmake.sh [--force]
set -euo pipefail

CMAKE_VERSION="4.4.4"
# From https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-SHA-256.txt
SHA256_MACOS_UNIVERSAL="4b7b73704b1db9b374e5c9ab8e17ac6148b817b6396ee75cd94a852cbac9d305"
SHA256_LINUX_X86_64="e5bb807f7728cb60cd8b27ebc97a2edb469b68655f21e844a600c3575b76f5bb"
SHA256_LINUX_AARCH64="a1b6cc63636a0e55c63257cf3315a8a5f129e42fade25db1afea4ff8ab06f25e"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOLCHAIN="${REPO_ROOT}/.toolchain"
PREFIX="${TOOLCHAIN}/cmake"
DL="${TOOLCHAIN}/downloads"
STAMP="${PREFIX}/.osef-cmake-${CMAKE_VERSION}"

log() { printf '[cmake] %s\n' "$*" >&2; }
die() { printf '[cmake] error: %s\n' "$*" >&2; exit 1; }

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

os="$(uname -s)"
arch="$(uname -m)"
case "$os" in
    Darwin)
        PLATFORM="macos-universal"
        SHA256="$SHA256_MACOS_UNIVERSAL"
        BINREL="CMake.app/Contents/bin"
        ;;
    Linux)
        case "$arch" in
            x86_64|amd64) PLATFORM="linux-x86_64"; SHA256="$SHA256_LINUX_X86_64" ;;
            aarch64|arm64) PLATFORM="linux-aarch64"; SHA256="$SHA256_LINUX_AARCH64" ;;
            *) die "no prebuilt CMake for Linux/${arch}" ;;
        esac
        BINREL="bin"
        ;;
    *) die "unsupported host OS: ${os}" ;;
esac

DIST="cmake-${CMAKE_VERSION}-${PLATFORM}"
TARBALL="${DIST}.tar.gz"
URL="https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/${TARBALL}"
REAL_BIN="${PREFIX}/${DIST}/${BINREL}"

if [ "$FORCE" = 0 ] && [ -f "$STAMP" ] && [ -x "${PREFIX}/bin/cmake" ] \
    && "${PREFIX}/bin/cmake" --version >/dev/null 2>&1; then
    log "already installed (${CMAKE_VERSION})"
    echo "${PREFIX}/bin/cmake"
    exit 0
fi

command -v curl >/dev/null 2>&1 || die "curl not found"

mkdir -p "$DL"
if [ ! -f "${DL}/${TARBALL}" ] || [ "$(sha256_of "${DL}/${TARBALL}")" != "$SHA256" ]; then
    rm -f "${DL}/${TARBALL}"
    log "downloading ${URL}"
    curl -fsSL --retry 3 -o "${DL}/${TARBALL}.part" "$URL" || die "download failed"
    mv "${DL}/${TARBALL}.part" "${DL}/${TARBALL}"
fi
got="$(sha256_of "${DL}/${TARBALL}")"
[ "$got" = "$SHA256" ] || die "sha256 mismatch for ${TARBALL}: got ${got}, want ${SHA256}"

log "extracting into ${PREFIX}"
rm -rf "$PREFIX"
mkdir -p "$PREFIX"
tar -xzf "${DL}/${TARBALL}" -C "$PREFIX"
[ -x "${REAL_BIN}/cmake" ] || die "cmake binary not found at ${REAL_BIN}/cmake"

# Stable paths that do not depend on the version or host layout.
mkdir -p "${PREFIX}/bin"
for tool in cmake ctest cpack; do
    if [ -x "${REAL_BIN}/${tool}" ]; then
        ln -sfn "../${DIST}/${BINREL}/${tool}" "${PREFIX}/bin/${tool}"
    fi
done

"${PREFIX}/bin/cmake" --version >/dev/null 2>&1 || die "installed cmake does not run"
touch "$STAMP"
log "installed: $("${PREFIX}/bin/cmake" --version | head -n 1)"
log "real binary: ${REAL_BIN}/cmake"
echo "${PREFIX}/bin/cmake"
