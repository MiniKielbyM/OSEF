#!/usr/bin/env bash
# Fetch the OSEF Editor's third-party dependencies into <repo>/editor/third_party:
#   imgui/   Dear ImGui (docking branch): core, glfw + opengl3 backends, imgui_stdlib
#   glfw/    GLFW source (built from source by editor/Makefile and editor/CMakeLists.txt)
#   fonts/   Inter (UI) and JetBrains Mono (code), both SIL Open Font License
#
# Every archive is pinned by version and sha256. Idempotent: a component is
# only (re)installed when its stamp file doesn't match the pinned version.
# Works on macOS and Linux; installs nothing outside editor/third_party.
#
# Usage: tools/bootstrap/editor-deps.sh [--force]
#
# If you bump a version, update the same URL + hash in editor/CMakeLists.txt.
set -euo pipefail

IMGUI_VERSION="1.93.0-docking"
IMGUI_URL="https://github.com/ocornut/imgui/archive/refs/tags/v${IMGUI_VERSION}.tar.gz"
IMGUI_SHA256="746c265540bd3aa109570f29492ef4d8f5b421dd9fd0049c35ca35f21496f1a5"

GLFW_VERSION="3.5.1"
GLFW_URL="https://github.com/glfw/glfw/releases/download/${GLFW_VERSION}/glfw-${GLFW_VERSION}.zip"
GLFW_SHA256="ea79bc5feffc254c87291980c2d0bce9acebb68c4983b79f961dcd2cb8a611a0"

INTER_VERSION="4.1"
INTER_URL="https://github.com/rsms/inter/releases/download/v${INTER_VERSION}/Inter-${INTER_VERSION}.zip"
INTER_SHA256="9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e"

JBMONO_VERSION="2.304"
JBMONO_URL="https://github.com/JetBrains/JetBrainsMono/releases/download/v${JBMONO_VERSION}/JetBrainsMono-${JBMONO_VERSION}.zip"
JBMONO_SHA256="6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
THIRD_PARTY="${REPO_ROOT}/editor/third_party"

log() { printf '[editor-deps] %s\n' "$*" >&2; }
die() { printf '[editor-deps] error: %s\n' "$*" >&2; exit 1; }

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

command -v curl >/dev/null 2>&1 || die "curl not found"
command -v tar >/dev/null 2>&1 || die "tar not found"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        die "need sha256sum or shasum"
    fi
}

unzip_to() {  # unzip_to <archive> <dir>
    if command -v unzip >/dev/null 2>&1; then
        unzip -q "$1" -d "$2"
    elif command -v python3 >/dev/null 2>&1; then
        python3 -I -m zipfile -e "$1" "$2"
    else
        die "need unzip or python3 to extract $1"
    fi
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/osef-editor-deps.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# fetch <name> <url> <sha256> -> prints the verified archive path
fetch() {
    local name="$1" url="$2" want="$3"
    local out="${WORK}/${name}.archive"
    log "downloading ${url}"
    curl -fsSL --retry 3 --connect-timeout 20 -o "$out" "$url" || die "download failed: ${url}"
    local got
    got="$(sha256_of "$out")"
    [ "$got" = "$want" ] || die "sha256 mismatch for ${name}: expected ${want}, got ${got}"
    printf '%s\n' "$out"
}

# up_to_date <dir> <stamp text>: 0 if <dir>/.osef-version matches
up_to_date() {
    [ "$FORCE" = 0 ] && [ -f "$1/.osef-version" ] && [ "$(cat "$1/.osef-version")" = "$2" ]
}

# install_dir <staged dir> <dest dir> <stamp text>: replaces dest atomically-ish
install_dir() {
    printf '%s\n' "$3" > "$1/.osef-version"
    rm -rf "$2"
    mkdir -p "$(dirname "$2")"
    mv "$1" "$2"
}

mkdir -p "$THIRD_PARTY"

# ---- Dear ImGui -------------------------------------------------------------
IMGUI_STAMP="imgui ${IMGUI_VERSION} ${IMGUI_SHA256}"
if up_to_date "${THIRD_PARTY}/imgui" "$IMGUI_STAMP"; then
    log "imgui ${IMGUI_VERSION} already installed"
else
    archive="$(fetch imgui "$IMGUI_URL" "$IMGUI_SHA256")"
    mkdir -p "${WORK}/imgui-x"
    tar -xzf "$archive" -C "${WORK}/imgui-x"
    src="${WORK}/imgui-x/imgui-${IMGUI_VERSION}"
    [ -f "${src}/imgui.h" ] || die "unexpected imgui archive layout"
    stage="${WORK}/imgui"
    mkdir -p "${stage}/backends" "${stage}/misc/cpp"
    cp "${src}/LICENSE.txt" "${src}"/imconfig.h "${src}"/imgui*.h "${src}"/imgui*.cpp \
       "${src}"/imstb_*.h "${stage}/"
    cp "${src}"/backends/imgui_impl_glfw.* "${src}"/backends/imgui_impl_opengl3* "${stage}/backends/"
    cp "${src}"/misc/cpp/imgui_stdlib.* "${stage}/misc/cpp/"
    install_dir "$stage" "${THIRD_PARTY}/imgui" "$IMGUI_STAMP"
    log "installed imgui ${IMGUI_VERSION}"
fi

# ---- GLFW -------------------------------------------------------------------
GLFW_STAMP="glfw ${GLFW_VERSION} ${GLFW_SHA256}"
if up_to_date "${THIRD_PARTY}/glfw" "$GLFW_STAMP"; then
    log "glfw ${GLFW_VERSION} already installed"
else
    archive="$(fetch glfw "$GLFW_URL" "$GLFW_SHA256")"
    mkdir -p "${WORK}/glfw-x"
    unzip_to "$archive" "${WORK}/glfw-x"
    stage="${WORK}/glfw-x/glfw-${GLFW_VERSION}"
    [ -f "${stage}/include/GLFW/glfw3.h" ] || die "unexpected glfw archive layout"
    # Docs, examples and tests aren't needed to build the library.
    rm -rf "${stage}/docs" "${stage}/examples" "${stage}/tests"
    install_dir "$stage" "${THIRD_PARTY}/glfw" "$GLFW_STAMP"
    log "installed glfw ${GLFW_VERSION}"
fi

# ---- Fonts --------------------------------------------------------------------
FONTS_STAMP="inter ${INTER_VERSION} ${INTER_SHA256}; jetbrains-mono ${JBMONO_VERSION} ${JBMONO_SHA256}"
if up_to_date "${THIRD_PARTY}/fonts" "$FONTS_STAMP"; then
    log "fonts already installed"
else
    stage="${WORK}/fonts"
    mkdir -p "$stage"

    archive="$(fetch inter "$INTER_URL" "$INTER_SHA256")"
    mkdir -p "${WORK}/inter-x"
    unzip_to "$archive" "${WORK}/inter-x"
    for f in Inter-Regular.ttf Inter-SemiBold.ttf; do
        [ -f "${WORK}/inter-x/extras/ttf/${f}" ] || die "Inter archive is missing extras/ttf/${f}"
        cp "${WORK}/inter-x/extras/ttf/${f}" "${stage}/"
    done
    cp "${WORK}/inter-x/LICENSE.txt" "${stage}/Inter-OFL.txt"

    archive="$(fetch jetbrains-mono "$JBMONO_URL" "$JBMONO_SHA256")"
    mkdir -p "${WORK}/jbm-x"
    unzip_to "$archive" "${WORK}/jbm-x"
    [ -f "${WORK}/jbm-x/fonts/ttf/JetBrainsMono-Regular.ttf" ] || die "JetBrains Mono archive layout changed"
    cp "${WORK}/jbm-x/fonts/ttf/JetBrainsMono-Regular.ttf" "${stage}/"
    cp "${WORK}/jbm-x/OFL.txt" "${stage}/JetBrainsMono-OFL.txt"

    install_dir "$stage" "${THIRD_PARTY}/fonts" "$FONTS_STAMP"
    log "installed Inter ${INTER_VERSION} and JetBrains Mono ${JBMONO_VERSION}"
fi

log "done: ${THIRD_PARTY}"
