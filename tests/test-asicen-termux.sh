#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
launcher=$root/packaging/termux/asicen-termux

fail_test()
{
    printf 'asicen-termux test: %s\n' "$1" >&2
    exit 1
}

[ -x "$launcher" ] || fail_test "launcher is not executable: $launcher"

if command -v shellcheck >/dev/null 2>&1; then
    shellcheck "$launcher" "$root/tests/test-asicen-termux.sh" ||
        fail_test 'shellcheck reported problems'
else
    printf '%s\n' 'asicen-termux test: shellcheck not found; skipping lint' >&2
fi

test_root=$(mktemp -d "${TMPDIR:-/tmp}/asicen-termux-test.XXXXXX")
cleanup()
{
    rm -rf "$test_root"
}
trap cleanup EXIT

fake_bin=$test_root/bin
mkdir "$fake_bin"

cat > "$fake_bin/termux-usb" <<'EOF'
#!/bin/sh
set -eu
[ "$#" -eq 3 ] && [ "$1" = -e ] || exit 91
callback=$2
usb=$3
case "$usb" in
    */usb-primary) exec 7<"$usb"; fd=7 ;;
    */usb-sibling) exec 8<"$usb"; fd=8 ;;
    *) exit 92 ;;
esac
exec sh "$callback" "$fd"
EOF
chmod 0755 "$fake_bin/termux-usb"

cat > "$fake_bin/asicend" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$#" > "$FAKE_ASICEND_LOG"
for argument do
    printf '[%s]\n' "$argument" >> "$FAKE_ASICEND_LOG"
done
previous=
for argument do
    case "$previous" in
        --fd)
            if [ -e "/proc/self/fd/$argument" ]; then
                printf '%s=%s\n' "$previous" "$(readlink "/proc/self/fd/$argument")" \
                    >> "$FAKE_ASICEND_FD_LOG"
            else
                printf '%s=CLOSED\n' "$previous" >> "$FAKE_ASICEND_FD_LOG"
            fi
            ;;
    esac
    previous=$argument
done
exit "${FAKE_ASICEND_EXIT:-0}"
EOF
chmod 0755 "$fake_bin/asicend"

usb_primary=$test_root/usb-primary
usb_sibling=$test_root/usb-sibling
firmware=$test_root/firmware-with-spaces.bin
runtime=$test_root/runtime-with-spaces
: > "$usb_primary"
: > "$usb_sibling"
: > "$firmware"

log=$test_root/asicend.log
fd_log=$test_root/asicend-fd.log

run_launcher()
{
    PATH="$fake_bin:$PATH" FAKE_ASICEND_LOG="$log" FAKE_ASICEND_FD_LOG="$fd_log" \
        sh "$launcher" "$@"
}

expect_reject()
{
    context=$1
    shift
    if run_launcher "$@" 2>/dev/null; then
        fail_test "$context: launcher accepted invalid arguments"
    fi
    return 0
}

assert_argv()
{
    context=$1
    shift
    expected=$test_root/expected.log
    actual=$test_root/actual.log
    : > "$expected"
    for argument do
        printf '[%s]\n' "$argument" >> "$expected"
    done
    tail -n +2 "$log" > "$actual"
    diff -u "$expected" "$actual" >/dev/null || fail_test "$context: argv mismatch"
    [ "$(head -n 1 "$log")" -eq "$#" ] ||
        fail_test "$context: argument count mismatch"
    return 0
}

assert_fd_open()
{
    line=$1
    context=$2
    grep -Fx -- "$line" "$fd_log" >/dev/null || fail_test "$context: missing $line"
    return 0
}

# Argument rejection happens before termux-usb is required, so no USB device or
# fake termux-usb is involved.
expect_reject 'missing --usb-device'
expect_reject 'three USB devices' --usb-device "$usb_primary" \
    --usb-device "$usb_sibling" --usb-device "$usb_primary"
expect_reject '--allow-lnb-power' --usb-device "$usb_primary" --allow-lnb-power
expect_reject 'unknown option' --usb-device "$usb_primary" --unknown
expect_reject '--fd is not a launcher option' --usb-device "$usb_primary" --fd 7
expect_reject '--usb-device without a value' --usb-device
expect_reject '--usb-device= form' --usb-device="$usb_primary"
expect_reject 'duplicate --model' --usb-device "$usb_primary" --model w3u3 --model s3u
expect_reject '--help combined with arguments' --help --usb-device "$usb_primary"

# Single-function model: one device becomes a single --fd only, and the exact
# fd termux-usb granted stays open for asicend.
: > "$log"
: > "$fd_log"
run_launcher --usb-device "$usb_primary"
assert_argv 'single-device' --fd 7
assert_fd_open "--fd=$usb_primary" 'single-device'

# Paired model: the first device is the primary and the second the sibling, and
# optional arguments are forwarded only when requested.
: > "$log"
: > "$fd_log"
run_launcher --usb-device "$usb_primary" --usb-device "$usb_sibling" \
    --model w3u3 --runtime-dir "$runtime" --instance termux.one --firmware "$firmware"
assert_argv 'paired' --fd 7 --fd 8 \
    --model w3u3 --runtime-dir "$runtime" --instance termux.one --firmware "$firmware"
assert_fd_open "--fd=$usb_primary" 'paired primary'
assert_fd_open "--fd=$usb_sibling" 'paired sibling'

# Optional arguments are absent from argv when they are not requested.
: > "$log"
: > "$fd_log"
run_launcher --usb-device "$usb_primary" --usb-device "$usb_sibling"
assert_argv 'paired without options' --fd 7 --fd 8

printf '%s\n' 'asicen-termux offline tests: PASS'
