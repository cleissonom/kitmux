#!/bin/sh
set -eu
[ "$#" -eq 1 ] || { echo 'Usage: build-release.sh vVERSION' >&2; exit 1; }
tag=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_version=$(sed -n 's/^#define KM_VERSION "\([^"]*\)"/\1/p' "$root/include/kitmux.h")
[ "$tag" = "v$source_version" ] || { echo 'Release tag does not match source version' >&2; exit 1; }
case "$(uname -s)" in
    Darwin) target_os=darwin; export MACOSX_DEPLOYMENT_TARGET=15.0 ;;
    Linux) target_os=linux ;;
    *) echo 'Unsupported release OS' >&2; exit 1 ;;
esac
case "$(uname -m)" in
    arm64|aarch64) target_arch=arm64 ;;
    x86_64) target_arch=x86_64 ;;
    *) echo 'Unsupported release architecture' >&2; exit 1 ;;
esac
work=${RELEASE_WORK:-$root/build-release-deps}
sh "$root/scripts/release-deps.sh" "$work"
work=$(CDPATH= cd -- "$work" && pwd)
prefix="$work/prefix"
libraries="$prefix/lib/libjson-c.a $prefix/lib/libyaml.a $prefix/lib/libcrypto.a"
feature_flags=
if [ "$target_os" = linux ]; then
    feature_flags=-D_DEFAULT_SOURCE
    libraries="$libraries $prefix/lib/libncursesw.a -ldl -pthread -lm"
else
    libraries="$libraries -lncurses"
fi
cd "$root"
make -B -j "${RELEASE_JOBS:-2}" BUILD="build-release-$target_os-$target_arch" \
    CPPFLAGS="-Iinclude -I$prefix/include $feature_flags" \
    CFLAGS='-std=c17 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -O2 -DNDEBUG -Wall -Wextra -Wpedantic' \
    LDLIBS="$libraries"
binary="$root/build-release-$target_os-$target_arch/kitmux"
sh "$root/scripts/verify-release.sh" "$binary" "$target_os" "$target_arch"
sh "$root/scripts/package-release.sh" "$binary" "$tag" "$target_os" "$target_arch" \
    "$work/licenses" "${RELEASE_OUTPUT:-$root/dist}"
