#!/usr/bin/env bash
# Fetch pinned Limine bootloader binaries + protocol header into .toolchain and
# build the Limine host utility (.toolchain/limine/limine) with the host cc.
#   .toolchain/limine           checkout of tag v11.4.1-binary
#   .toolchain/limine-protocol  checkout of limine-protocol at a pinned commit
# Idempotent; installs nothing outside the repo.
#
# Usage: tools/bootstrap/limine.sh
set -euo pipefail

LIMINE_REPO="https://github.com/limine-bootloader/limine.git"
LIMINE_TAG="v11.4.1-binary"
LIMINE_COMMIT="5be26a73d7b7b4d4477d18be94e1d16e615adf56"   # what $LIMINE_TAG points to

PROTOCOL_REPO="https://github.com/limine-bootloader/limine-protocol.git"
PROTOCOL_COMMIT="3a0526b700e356f0eac1b71a77697b3fd1c707a3"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOLCHAIN="${REPO_ROOT}/.toolchain"
LIMINE_DIR="${TOOLCHAIN}/limine"
PROTOCOL_DIR="${TOOLCHAIN}/limine-protocol"

log() { printf '[limine] %s\n' "$*" >&2; }
die() { printf '[limine] error: %s\n' "$*" >&2; exit 1; }

command -v git >/dev/null 2>&1 || die "git not found"
command -v make >/dev/null 2>&1 || die "make not found"
mkdir -p "$TOOLCHAIN"

head_of() { git -C "$1" rev-parse --verify --quiet HEAD 2>/dev/null || true; }

# ---- Limine binaries --------------------------------------------------------
if [ -d "$LIMINE_DIR" ] && [ ! -d "$LIMINE_DIR/.git" ]; then
    die "${LIMINE_DIR} exists but is not a git checkout; remove it and re-run"
fi
if [ ! -d "$LIMINE_DIR" ]; then
    log "cloning ${LIMINE_REPO} @ ${LIMINE_TAG}"
    git clone --quiet --depth 1 --branch "$LIMINE_TAG" "$LIMINE_REPO" "$LIMINE_DIR"
fi
if [ "$(head_of "$LIMINE_DIR")" != "$LIMINE_COMMIT" ]; then
    log "checking out ${LIMINE_TAG}"
    git -C "$LIMINE_DIR" fetch --quiet --depth 1 origin "refs/tags/${LIMINE_TAG}:refs/tags/${LIMINE_TAG}"
    git -C "$LIMINE_DIR" -c advice.detachedHead=false checkout --quiet "$LIMINE_TAG"
fi
[ "$(head_of "$LIMINE_DIR")" = "$LIMINE_COMMIT" ] \
    || die "${LIMINE_DIR} HEAD is $(head_of "$LIMINE_DIR"), expected ${LIMINE_TAG} (${LIMINE_COMMIT})"
tag_commit="$(git -C "$LIMINE_DIR" rev-parse --verify --quiet "refs/tags/${LIMINE_TAG}^{commit}" || true)"
[ -z "$tag_commit" ] || [ "$tag_commit" = "$LIMINE_COMMIT" ] \
    || die "tag ${LIMINE_TAG} points to ${tag_commit}, expected ${LIMINE_COMMIT} (upstream retagged?)"
# Refuse to build from locally modified tracked files.
if ! git -C "$LIMINE_DIR" diff --quiet HEAD --; then
    die "${LIMINE_DIR} has local modifications; run 'git -C ${LIMINE_DIR} checkout -- .'"
fi
for f in limine.c Makefile limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin BOOTX64.EFI; do
    [ -f "${LIMINE_DIR}/${f}" ] || die "missing ${LIMINE_DIR}/${f}"
done

# Host utility (limine bios-install etc.). make rebuilds only if limine.c changed.
log "building host utility"
make -s -C "$LIMINE_DIR" limine >/dev/null || die "building ${LIMINE_DIR}/limine failed"
"${LIMINE_DIR}/limine" --version >/dev/null 2>&1 || die "${LIMINE_DIR}/limine does not run"

# ---- Limine protocol header -------------------------------------------------
if [ -d "$PROTOCOL_DIR" ] && [ ! -d "$PROTOCOL_DIR/.git" ]; then
    die "${PROTOCOL_DIR} exists but is not a git checkout; remove it and re-run"
fi
if [ ! -d "$PROTOCOL_DIR" ]; then
    log "cloning ${PROTOCOL_REPO}"
    git clone --quiet "$PROTOCOL_REPO" "$PROTOCOL_DIR"
fi
if [ "$(head_of "$PROTOCOL_DIR")" != "$PROTOCOL_COMMIT" ]; then
    log "checking out limine-protocol ${PROTOCOL_COMMIT}"
    if ! git -C "$PROTOCOL_DIR" cat-file -e "${PROTOCOL_COMMIT}^{commit}" 2>/dev/null; then
        git -C "$PROTOCOL_DIR" fetch --quiet origin "$PROTOCOL_COMMIT" \
            || git -C "$PROTOCOL_DIR" fetch --quiet origin
    fi
    git -C "$PROTOCOL_DIR" -c advice.detachedHead=false checkout --quiet "$PROTOCOL_COMMIT"
fi
[ "$(head_of "$PROTOCOL_DIR")" = "$PROTOCOL_COMMIT" ] \
    || die "${PROTOCOL_DIR} HEAD is $(head_of "$PROTOCOL_DIR"), expected ${PROTOCOL_COMMIT}"
if ! git -C "$PROTOCOL_DIR" diff --quiet HEAD --; then
    die "${PROTOCOL_DIR} has local modifications; run 'git -C ${PROTOCOL_DIR} checkout -- .'"
fi
[ -f "${PROTOCOL_DIR}/include/limine.h" ] || die "missing ${PROTOCOL_DIR}/include/limine.h"

log "limine $("${LIMINE_DIR}/limine" --version 2>/dev/null | head -n 1) (${LIMINE_TAG}), protocol ${PROTOCOL_COMMIT}"
echo "${LIMINE_DIR}/limine"
echo "${LIMINE_DIR}"
echo "${PROTOCOL_DIR}/include/limine.h"
