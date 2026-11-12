#!/bin/sh
# Build pinned dependency archives inside an isolated prefix; never install globally.
set -eu
[ "$#" -eq 1 ] || { echo 'Usage: release-deps.sh WORK_DIRECTORY' >&2; exit 1; }
mkdir -p "$1"
work=$(CDPATH= cd -- "$1" && pwd)
prefix="$work/prefix"
target_os=$(uname -s)
if [ "$target_os" = Darwin ]; then export MACOSX_DEPLOYMENT_TARGET=15.0; fi
jobs=${RELEASE_JOBS:-2}
downloads=${RELEASE_DOWNLOADS:-$work/downloads}
mkdir -p "$downloads" "$work/licenses" "$work/sources"

fetch() {
    source_name=$1 source_url=$2 source_hash=$3
    archive="$downloads/$source_name.tar.gz"
    if [ ! -f "$archive" ]; then
        curl --fail --location --retry 3 --proto '=https' --tlsv1.2 "$source_url" -o "$archive.part"
        mv "$archive.part" "$archive"
    fi
    actual=$(shasum -a 256 "$archive" | awk '{ print $1 }')
    [ "$actual" = "$source_hash" ] || { echo "Checksum mismatch: $source_name" >&2; exit 1; }
    rm -rf "$work/sources/$source_name"
    tar -xzf "$archive" -C "$work/sources"
}

build_json() {
    fetch json-c-0.19 https://s3.amazonaws.com/json-c_releases/releases/json-c-0.19.tar.gz \
        37ad0249902e301bd9052bf712e511fcc6acff4ecaad4b5900aad9ce564e26de
    cmake -S "$work/sources/json-c-0.19" -B "$work/json-build" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-}" \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DBUILD_APPS=OFF
    cmake --build "$work/json-build" --parallel "$jobs"
    cmake --install "$work/json-build"
    cp "$work/sources/json-c-0.19/COPYING" "$work/licenses/json-c.txt"
}

build_yaml() {
    fetch yaml-0.2.5 https://pyyaml.org/download/libyaml/yaml-0.2.5.tar.gz \
        c642ae9b75fee120b2d96c712538bd2cf283228d2337df2cf2988e3c02678ef4
    (cd "$work/sources/yaml-0.2.5"
        ./configure --prefix="$prefix" --disable-shared --enable-static
        make -j "$jobs"
        make install)
    cp "$work/sources/yaml-0.2.5/License" "$work/licenses/libyaml.txt"
}

build_crypto() {
    fetch openssl-3.5.9 https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz \
        603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
    (cd "$work/sources/openssl-3.5.9"
        ./Configure --prefix="$prefix" --libdir=lib no-shared no-module no-tests
        make -j "$jobs" build_libs
        make install_dev)
    cp "$work/sources/openssl-3.5.9/LICENSE.txt" "$work/licenses/openssl.txt"
    if [ -f "$work/sources/openssl-3.5.9/NOTICE" ]; then
        cp "$work/sources/openssl-3.5.9/NOTICE" "$work/licenses/openssl-NOTICE.txt"
    fi
}

build_curses() {
    fetch ncurses-6.6 https://invisible-island.net/archives/ncurses/ncurses-6.6.tar.gz \
        355b4cbbed880b0381a04c46617b7656e362585d52e9cf84a67e2009b749ff11
    (cd "$work/sources/ncurses-6.6"
        ./configure --prefix="$prefix" --with-normal --without-shared --without-debug \
            --without-ada --without-cxx --without-cxx-binding --without-progs --without-tests \
            --enable-widec --enable-overwrite --with-terminfo-dirs=/etc/terminfo:/lib/terminfo:/usr/share/terminfo
        make -j "$jobs"
        make install.libs install.includes)
    cp "$work/sources/ncurses-6.6/COPYING" "$work/licenses/ncurses.txt"
}

# Match the stamp to target and pinned inputs, so another architecture cannot reuse it.
stamp="$(uname -sm)-${MACOSX_DEPLOYMENT_TARGET:-glibc2.35}-json0.19-yaml0.2.5-crypto3.5.9-curses6.6"
if [ -f "$work/complete" ] && [ "$(cat "$work/complete")" = "$stamp" ]; then exit 0; fi
build_json
build_yaml
build_crypto
if [ "$target_os" = Linux ]; then build_curses; fi
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sed -n '1,/\*\//p' "$project_root/src/adapters_toml_vendor.c" > "$work/licenses/tomlc99.txt"
printf '%s\n' "$stamp" > "$work/complete"
