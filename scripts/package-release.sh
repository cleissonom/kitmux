#!/bin/sh
set -eu

[ "$#" -eq 6 ] || { echo 'Usage: package-release.sh BINARY TAG OS ARCH LICENSES OUTPUT' >&2; exit 1; }
binary=$1 tag=$2 target_os=$3 target_arch=$4 license_dir=$5 output_dir=$6
case "$target_os:$target_arch" in
    darwin:arm64|darwin:x86_64|linux:arm64|linux:x86_64) ;;
    *) echo 'Unsupported release target' >&2; exit 1 ;;
esac
printf '%s\n' "$tag" | LC_ALL=C awk '/^v[0-9]+\.[0-9]+\.[0-9]+$/ { ok=1 } END { exit !ok }' || {
    echo 'Invalid release version tag' >&2; exit 1;
}
[ "$("$binary" --version)" = "kitmux ${tag#v}" ] || {
    echo 'Release tag does not match executable version' >&2; exit 1;
}
for dependency in json-c libyaml openssl tomlc99; do
    [ -s "$license_dir/$dependency.txt" ] || { echo "Missing $dependency license" >&2; exit 1; }
done
if [ "$target_os" = linux ]; then
    [ -s "$license_dir/ncurses.txt" ] || { echo 'Missing ncurses license' >&2; exit 1; }
fi
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
stage=$(mktemp -d "${TMPDIR:-/tmp}/kitmux-package.XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM
name="kitmux-$tag-$target_os-$target_arch"
mkdir -p "$stage/$name/licenses"
install -m 755 "$binary" "$stage/$name/kitmux"
cp "$project_root/LICENSE" "$project_root/THIRD_PARTY.md" "$stage/$name/"
cp "$license_dir/"*.txt "$stage/$name/licenses/"
mkdir -p "$output_dir"
tar -czf "$stage/$name.tar.gz" -C "$stage" "$name"
mv "$stage/$name.tar.gz" "$output_dir/$name.tar.gz"
printf '%s\n' "$output_dir/$name.tar.gz"
