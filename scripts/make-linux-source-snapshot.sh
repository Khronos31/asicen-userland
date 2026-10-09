#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

usage() { printf '%s\n' 'usage: make-linux-source-snapshot.sh --output FILE.tar.gz'; }
output=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --output) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; output=$2; shift 2 ;;
        --help) usage; exit 0 ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done
[ -n "$output" ] || { usage >&2; exit 2; }
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
for tool in git tar gzip sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || {
        printf 'required tool missing: %s\n' "$tool" >&2
        exit 1
    }
done
parent=$(dirname -- "$output")
mkdir -p "$parent"
parent=$(CDPATH='' cd -- "$parent" && pwd -P)
output="$parent/$(basename -- "$output")"
case "$output" in "$root"/*) printf '%s\n' 'snapshot output must be outside the source tree' >&2; exit 1 ;; esac
[ ! -e "$output" ] && [ ! -L "$output" ] || { printf 'snapshot output already exists: %s\n' "$output" >&2; exit 1; }
work=$(mktemp -d "$parent/.asicen-source-snapshot.XXXXXX")
cleanup() { rm -rf -- "$work"; }
trap cleanup EXIT HUP INT TERM

git -C "$root" ls-files -z > "$work/paths.unsorted"
git -C "$root" ls-files > "$work/path-lines"
cat >> "$work/explicit-paths" <<'EOF'
scripts/audit-linux-candidate.py
scripts/build-linux-ifd.sh
scripts/build-linux-static.sh
scripts/make-linux-source-snapshot.sh
scripts/package-linux-variant.py
packaging/pcsc/reader.conf.d/asicen-reader.conf.in
userland/tests/static_distribution_tests.py
EOF
while IFS= read -r path; do
    [ -e "$root/$path" ] || { printf 'explicit source path missing: %s\n' "$path" >&2; exit 1; }
done < "$work/explicit-paths"
: > "$work/path-lines.keep"
while IFS= read -r path; do
    case "$path" in
        firmware/*|*/firmware/*)
            # Checked out for distribution packaging. Corresponding source omits it.
            continue ;;
        .git/*|*/.git/*|secrets.yaml|*/secrets.yaml|.storage/*|*/.storage/*|.ssh/*|*/.ssh/*|*.pcap|*.ts)
            printf 'excluded/private material is indexed for the source snapshot: %s\n' "$path" >&2
            exit 1 ;;
    esac
    printf '%s\n' "$path" >> "$work/path-lines.keep"
done < "$work/path-lines"
tr '\n' '\0' < "$work/path-lines.keep" > "$work/paths.keep.nul"
cat "$work/explicit-paths" | tr '\n' '\0' > "$work/explicit-paths.nul"
cat "$work/paths.keep.nul" "$work/explicit-paths.nul" | sort -zu > "$work/paths"
tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
    --no-recursion --null -C "$root" --files-from="$work/paths" \
    -cf "$work/snapshot.tar"
gzip -n -c "$work/snapshot.tar" > "$work/snapshot.tar.gz"
tar -tzf "$work/snapshot.tar.gz" | grep -E '(^|/)firmware/|asicen-loader\.bin' && {
    printf '%s\n' 'firmware path leaked into corresponding-source archive' >&2; exit 1;
}
mv -- "$work/snapshot.tar.gz" "$output"
sha256sum "$output"
