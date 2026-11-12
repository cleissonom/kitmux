#!/bin/sh
set -eu
[ "$#" -eq 3 ] || { echo 'Usage: verify-release.sh BINARY OS ARCH' >&2; exit 1; }
binary=$1 target_os=$2 target_arch=$3
description=$(file -b "$binary")
case "$target_os" in
    darwin) printf '%s\n' "$description" | awk '/^Mach-O 64-bit executable / { ok=1 } END { exit !ok }' ;;
    linux) printf '%s\n' "$description" | awk '/^ELF 64-bit / { ok=1 } END { exit !ok }' ;;
    *) exit 1 ;;
esac
case "$target_arch" in
    arm64) printf '%s\n' "$description" | awk '/arm64|aarch64/ { ok=1 } END { exit !ok }' ;;
    x86_64) printf '%s\n' "$description" | awk '/x86_64|x86-64/ { ok=1 } END { exit !ok }' ;;
    *) exit 1 ;;
esac
if [ "$target_os" = darwin ]; then
    # No Homebrew, build-prefix, or other non-system load commands may survive.
    dependencies=$(otool -L "$binary")
    printf '%s\n' "$dependencies" | awk 'NR > 1 && $1 !~ /^\/usr\/lib\// && $1 !~ /^\/System\/Library\// { bad=1; print > "/dev/stderr" } END { exit bad }'
    build_info=$(xcrun vtool -show-build "$binary")
    printf '%s\n' "$build_info" | awk '$1 == "minos" {
        split($2, version, "."); seen=1;
        if (version[1] > 15 || (version[1] == 15 && version[2] > 0)) bad=1;
    } END { if (!seen || bad) print "Requires macOS newer than 15.0" > "/dev/stderr"; exit !seen || bad }'
else
    # Library allowlist also catches accidentally dynamic third-party libraries.
    dependencies=$(readelf -d "$binary")
    printf '%s\n' "$dependencies" | awk '/NEEDED/ && $NF !~ /^\[(libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|ld-linux.*\.so.*)\]$/ { bad=1; print > "/dev/stderr" } END { exit bad }'
    versions=$(readelf --version-info "$binary")
    printf '%s\n' "$versions" | awk '{
        while (match($0, /GLIBC_[0-9]+\.[0-9]+/)) {
            version=substr($0, RSTART+6, RLENGTH-6); split(version, number, ".");
            if (number[1] > 2 || (number[1] == 2 && number[2] > 35)) bad=1;
            $0=substr($0, RSTART+RLENGTH);
        }
    } END { if (bad) print "Requires glibc newer than 2.35" > "/dev/stderr"; exit bad }'
fi
"$binary" --version
"$binary" --help > /dev/null
