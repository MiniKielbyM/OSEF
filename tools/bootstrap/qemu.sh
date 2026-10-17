#!/usr/bin/env bash
# Build qemu-system-x86_64 (+ bundled EDK2/OVMF + SeaBIOS firmware) from source into
# <repo>/.toolchain/qemu, with no Homebrew/MacPorts/sudo.
#
# Layout (all under <repo>/.toolchain, gitignored):
#   venv/         Python venv providing meson + ninja
#   qemu-src/     downloads (dl/), extracted sources, build dirs, step stamps
#   qemu-deps/    pkgconf, glib (+pcre2/libffi/proxy-libintl fallbacks), pixman  (static libs)
#   qemu/         final install prefix: bin/qemu-system-x86_64, share/qemu/*.fd ...
#
# All third-party deps are linked statically, so the resulting binary only depends on
# macOS system libraries/frameworks (no DYLD_* hacks needed).
#
# Idempotent: each finished step leaves a stamp in qemu-src/.stamps and is skipped on
# re-runs. Delete a stamp (or the whole .toolchain/qemu-src) to force a rebuild.
#
# Usage: tools/bootstrap/qemu.sh          (from any cwd)
#        JOBS=4 tools/bootstrap/qemu.sh   (override parallelism, default 8)
set -euo pipefail

# ---------------------------------------------------------------- versions / checksums
QEMU_VER=11.1.2
QEMU_SHA256=731b5681e4bb18be313231579b8efd0296c5b015fa36dc533874b639ba838016
GLIB_VER=2.88.4
GLIB_SHA256=25fd169f3b2f91fe2391ad297883ce0fcf30c69fc789ffd1c9672d1332b1c5a8
PIXMAN_VER=0.46.4
PIXMAN_SHA256=d09c44ebc3bd5bee7021c79f922fe8fb2fb57f7320f55e97ff9914d2346a591c
PKGCONF_VER=3.0.9
PKGCONF_SHA256=6ab2d3b8087dcf500e34003a6a8934dca55d0a3d6066aeafb03c352db3e68bfd
MESON_VER=1.11.1
NINJA_VER=1.13.2
PIP_VER=26.0.1
TOMLI_VER=2.2.1   # QEMU configure needs tomli on Python < 3.11
WHEEL_VER=0.45.1  # QEMU mkvenv (offline, --disable-download) needs wheel preinstalled

JOBS="${JOBS:-8}"

# ---------------------------------------------------------------- paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TC="$ROOT/.toolchain"
VENV="$TC/venv"
SRC="$TC/qemu-src"
DL="$SRC/dl"
STAMPS="$SRC/.stamps"
DEPS="$TC/qemu-deps"
PREFIX="$TC/qemu"

log()  { printf '\n\033[1;34m[qemu.sh]\033[0m %s\n' "$*"; }
die()  { printf '\n\033[1;31m[qemu.sh] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }
trap 'die "failed at line $LINENO (command: $BASH_COMMAND)"' ERR

have_stamp() { [ -f "$STAMPS/$1" ]; }
put_stamp()  { mkdir -p "$STAMPS"; date > "$STAMPS/$1"; }

[ "$(uname -s)" = Darwin ] || log "warning: this script was written for macOS hosts"
command -v python3 >/dev/null || die "python3 not found"
command -v cc >/dev/null || die "C compiler (cc) not found; install Xcode Command Line Tools"
command -v curl >/dev/null || die "curl not found"

mkdir -p "$DL" "$STAMPS" "$DEPS" "$PREFIX"

# Clean, controlled environment: only our venv + deps + system tools.
export PATH="$DEPS/bin:$VENV/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export PKG_CONFIG="$DEPS/bin/pkg-config"
export PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig:$DEPS/share/pkgconfig"
unset PKG_CONFIG_PATH CFLAGS CXXFLAGS LDFLAGS CPPFLAGS DYLD_LIBRARY_PATH DYLD_FALLBACK_LIBRARY_PATH || true
export CC=cc CXX=c++ OBJC=cc
# Deps are static and get linked into qemu; keep their deployment target consistent.
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-$(sw_vers -productVersion | cut -d. -f1).0}"

# fetch <url> <file> <sha256>
fetch() {
    local url="$1" file="$DL/$2" sum="$3"
    if [ -f "$file" ] && [ "$(shasum -a 256 "$file" | cut -d' ' -f1)" = "$sum" ]; then
        return 0
    fi
    log "downloading $url"
    curl -fL --retry 3 -o "$file.part" "$url"
    local got; got="$(shasum -a 256 "$file.part" | cut -d' ' -f1)"
    [ "$got" = "$sum" ] || { rm -f "$file.part"; die "checksum mismatch for $2: got $got want $sum"; }
    mv "$file.part" "$file"
}

# unpack <archive> <dirname>  (into $SRC, only if not already extracted)
unpack() {
    local archive="$DL/$1" dir="$SRC/$2"
    if [ -d "$dir" ] && have_stamp "unpack-$2"; then return 0; fi
    rm -rf "$dir"
    log "extracting $1"
    tar -xf "$archive" -C "$SRC"
    [ -d "$dir" ] || die "expected $dir after extracting $1"
    put_stamp "unpack-$2"
}

# ---------------------------------------------------------------- 1. venv: meson + ninja
if ! have_stamp "venv-meson$MESON_VER-ninja$NINJA_VER-tomli$TOMLI_VER-wheel$WHEEL_VER" || ! [ -x "$VENV/bin/meson" ] || ! [ -x "$VENV/bin/ninja" ]; then
    log "creating Python venv with meson $MESON_VER + ninja $NINJA_VER + tomli $TOMLI_VER"
    [ -x "$VENV/bin/python3" ] || python3 -m venv "$VENV"
    "$VENV/bin/python3" -m pip install -q --disable-pip-version-check "pip==$PIP_VER"
    "$VENV/bin/python3" -m pip install -q --disable-pip-version-check "meson==$MESON_VER" "ninja==$NINJA_VER" "tomli==$TOMLI_VER" "wheel==$WHEEL_VER"
    put_stamp "venv-meson$MESON_VER-ninja$NINJA_VER-tomli$TOMLI_VER-wheel$WHEEL_VER"
fi
"$VENV/bin/meson" --version >/dev/null
"$VENV/bin/ninja" --version >/dev/null

# ---------------------------------------------------------------- 2. pkgconf
if ! have_stamp "pkgconf-$PKGCONF_VER" || ! [ -x "$DEPS/bin/pkgconf" ]; then
    fetch "https://distfiles.ariadne.space/pkgconf/pkgconf-$PKGCONF_VER.tar.xz" \
          "pkgconf-$PKGCONF_VER.tar.xz" "$PKGCONF_SHA256"
    unpack "pkgconf-$PKGCONF_VER.tar.xz" "pkgconf-$PKGCONF_VER"
    log "building pkgconf $PKGCONF_VER"
    rm -rf "$SRC/build-pkgconf"; mkdir -p "$SRC/build-pkgconf"
    (
        cd "$SRC/build-pkgconf"
        "$SRC/pkgconf-$PKGCONF_VER/configure" --prefix="$DEPS" \
            --disable-shared --enable-static \
            --with-pkg-config-dir="$DEPS/lib/pkgconfig:$DEPS/share/pkgconfig" \
            --with-system-libdir=/usr/lib --with-system-includedir=/usr/include
        make -j"$JOBS"
        make install
    )
    put_stamp "pkgconf-$PKGCONF_VER"
fi
# pkg-config wrapper: always answer with static link flags (all our deps are static).
cat > "$DEPS/bin/pkg-config.tmp" <<EOF
#!/bin/sh
exec "$DEPS/bin/pkgconf" --static "\$@"
EOF
chmod +x "$DEPS/bin/pkg-config.tmp"; mv "$DEPS/bin/pkg-config.tmp" "$DEPS/bin/pkg-config"

MESON_COMMON=(--prefix="$DEPS" --libdir=lib --buildtype=release
              -Ddefault_library=static --wrap-mode=default)

# ---------------------------------------------------------------- 3. glib
if ! have_stamp "glib-$GLIB_VER" || ! [ -f "$DEPS/lib/pkgconfig/glib-2.0.pc" ]; then
    fetch "https://download.gnome.org/sources/glib/${GLIB_VER%.*}/glib-$GLIB_VER.tar.xz" \
          "glib-$GLIB_VER.tar.xz" "$GLIB_SHA256"
    unpack "glib-$GLIB_VER.tar.xz" "glib-$GLIB_VER"
    log "building glib $GLIB_VER (static, with pcre2/libffi/proxy-libintl wrap fallbacks)"
    rm -rf "$SRC/build-glib"
    meson setup "$SRC/build-glib" "$SRC/glib-$GLIB_VER" "${MESON_COMMON[@]}" \
        -Dtests=false -Dintrospection=disabled -Ddocumentation=false -Dman-pages=disabled \
        -Dnls=disabled -Dlibmount=disabled -Dselinux=disabled -Dsysprof=disabled \
        -Dlibelf=disabled -Dglib_debug=disabled -Dglib_assert=false -Dglib_checks=true \
        -Dforce_fallback_for=libffi,pcre2,proxy-libintl
    ninja -C "$SRC/build-glib" -j"$JOBS"
    meson install -C "$SRC/build-glib" --quiet
    put_stamp "glib-$GLIB_VER"
fi

# ---------------------------------------------------------------- 4. pixman
if ! have_stamp "pixman-$PIXMAN_VER" || ! [ -f "$DEPS/lib/pkgconfig/pixman-1.pc" ]; then
    fetch "https://cairographics.org/releases/pixman-$PIXMAN_VER.tar.gz" \
          "pixman-$PIXMAN_VER.tar.gz" "$PIXMAN_SHA256"
    unpack "pixman-$PIXMAN_VER.tar.gz" "pixman-$PIXMAN_VER"
    log "building pixman $PIXMAN_VER (static)"
    rm -rf "$SRC/build-pixman"
    meson setup "$SRC/build-pixman" "$SRC/pixman-$PIXMAN_VER" "${MESON_COMMON[@]}" \
        -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled -Dopenmp=disabled
    ninja -C "$SRC/build-pixman" -j"$JOBS"
    meson install -C "$SRC/build-pixman" --quiet
    put_stamp "pixman-$PIXMAN_VER"
fi

# ---------------------------------------------------------------- 5. QEMU
QEMU_BIN="$PREFIX/bin/qemu-system-x86_64"
if ! have_stamp "qemu-$QEMU_VER" || ! [ -x "$QEMU_BIN" ]; then
    fetch "https://download.qemu.org/qemu-$QEMU_VER.tar.xz" "qemu-$QEMU_VER.tar.xz" "$QEMU_SHA256"
    unpack "qemu-$QEMU_VER.tar.xz" "qemu-$QEMU_VER"
    log "configuring QEMU $QEMU_VER"
    rm -rf "$SRC/build-qemu"; mkdir -p "$SRC/build-qemu"
    (
        cd "$SRC/build-qemu"
        "$SRC/qemu-$QEMU_VER/configure" \
            --prefix="$PREFIX" \
            --target-list=x86_64-softmmu \
            --python="$VENV/bin/python3" \
            --ninja="$VENV/bin/ninja" \
            --disable-docs --disable-werror \
            --disable-download --disable-fdt --disable-slirp \
            --disable-gtk --disable-sdl --disable-vte --disable-opengl \
            --disable-curl --disable-gnutls --disable-nettle --disable-gcrypt \
            --disable-libssh --disable-libusb --disable-usb-redir --disable-spice \
            --disable-curses --disable-zstd --disable-bzip2 --disable-lzo --disable-snappy \
            --disable-png --disable-vnc-sasl --disable-vnc-jpeg \
            --disable-rust --disable-guest-agent --disable-tools \
            --enable-pixman --enable-vnc --enable-hvf
        log "building QEMU $QEMU_VER (this takes a while)"
        make -j"$JOBS"
        make install
    )
    put_stamp "qemu-$QEMU_VER"
fi

# ---------------------------------------------------------------- sanity checks
[ -x "$QEMU_BIN" ] || die "missing $QEMU_BIN"
OVMF="$PREFIX/share/qemu/edk2-x86_64-code.fd"
[ -s "$OVMF" ] || die "missing OVMF firmware $OVMF"
[ -s "$PREFIX/share/qemu/bios-256k.bin" ] || die "missing SeaBIOS $PREFIX/share/qemu/bios-256k.bin"

# Every linked dylib must be a system one (all deps are static).
bad="$(otool -L "$QEMU_BIN" | tail -n +2 | awk '{print $1}' \
       | grep -v -E '^(/usr/lib/|/System/Library/)' || true)"
[ -z "$bad" ] || die "qemu links non-system dylibs: $bad"

"$QEMU_BIN" --version | head -1

cat <<EOF

QEMU ready:
  binary   : $QEMU_BIN
  img tool : (qemu-img not built; --disable-tools)
  OVMF     : $OVMF  (UEFI code, use with -drive if=pflash,format=raw,readonly=on,file=...)
  SeaBIOS  : $PREFIX/share/qemu/bios-256k.bin  (default legacy BIOS)

Examples:
  BIOS : $QEMU_BIN -m 256 -display none -serial stdio -no-reboot -cdrom os-bios.iso
  UEFI : $QEMU_BIN -m 256 -display none -serial stdio -no-reboot \\
           -drive if=pflash,format=raw,readonly=on,file=$OVMF -cdrom os-uefi.iso
EOF
