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

test_root=$(mktemp -d "${TMPDIR:-/tmp}/asicen-termux-test.XXXXXX")
cleanup()
{
    if [ -n "${signal_latch-}" ]; then
        rm -f "$signal_latch" "$signal_tmp"
    fi
    find "$test_root" -depth -delete
}
trap cleanup EXIT

assert_equal()
{
    expected=$1
    actual=$2
    context=$3
    [ "$actual" = "$expected" ] || fail_test "$context: expected <$expected>, got <$actual>"
}

assert_file_exists()
{
    file=$1
    context=$2
    [ -e "$file" ] || fail_test "$context: missing $file"
}

assert_file_absent()
{
    file=$1
    context=$2
    [ ! -e "$file" ] || fail_test "$context: unexpected $file"
}

assert_log_line()
{
    line=$1
    context=$2
    grep -Fx "$line" "$log" >/dev/null || fail_test "$context: missing log line $line"
}

fake_bin=$test_root/bin
mkdir "$fake_bin"
cat > "$fake_bin/termux-usb" <<'EOF'
#!/bin/sh
set -eu
[ "$#" -eq 3 ] && [ "$1" = -e ] || exit 91
callback=$2
usb=$3
case "$usb" in
    */usb-one) exec 7<"$usb"; fd=7; name=one ;;
    */usb-two) exec 8<"$usb"; fd=8; name=two ;;
    *) exit 92 ;;
esac
if [ -n "${FAKE_TERMUX_PID_DIR-}" ]; then
    printf '%s\n' "$$" > "$FAKE_TERMUX_PID_DIR/$name"
fi
if [ "${FAKE_FAIL_USB-}" = "$usb" ]; then
    exit 17
fi
# An asynchronous POSIX-shell child inherits ignored INT/QUIT.  Reset the
# dispositions before exec so each callback participates in the signal matrix
# like the foreground callback process in Termux.
perl -e '$SIG{INT} = "DEFAULT"; $SIG{TERM} = "DEFAULT"; $SIG{HUP} = "DEFAULT"; exec @ARGV or die "exec: $!"' \
    -- sh "$callback" "$fd" &
callback_pid=$!
if [ -n "${FAKE_CALLBACK_PID_DIR-}" ]; then
    printf '%s\n' "$callback_pid" > "$FAKE_CALLBACK_PID_DIR/$name"
fi
forward_signal()
{
    signal=$1
    kill "-$signal" "$callback_pid" 2>/dev/null || :
    if [ "${FAKE_TERMUX_EXIT_ON_SIGNAL-0}" -eq 1 ]; then
        case "$signal" in
            INT) exit 130 ;;
            TERM) exit 143 ;;
            HUP) exit 129 ;;
        esac
    fi
}
trap 'forward_signal INT' INT
trap 'forward_signal TERM' TERM
trap 'forward_signal HUP' HUP
if [ "${FAKE_EARLY_EXIT_USB-}" = "$usb" ]; then
    ready_file=${FAKE_ASICEND_READY:-$FAKE_ASICEND_ENDPOINT}
    while [ ! -e "$ready_file" ]; do
        sleep 0.1
    done
    exit 0
fi
callback_status=0
wait "$callback_pid" || callback_status=$?
trap - INT TERM HUP
exit "$callback_status"
EOF
chmod 0755 "$fake_bin/termux-usb"
fast_bin=$test_root/fast-bin
mkdir "$fast_bin"
cat > "$fast_bin/sleep" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod 0755 "$fast_bin/sleep"
cat > "$fake_bin/asicend" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$#" > "$FAKE_ASICEND_LOG"
printf '%s\n' "$$" > "$FAKE_ASICEND_PID"
for argument do
    printf '[%s]\n' "$argument" >> "$FAKE_ASICEND_LOG"
done
previous=
for argument do
    if [ "$previous" = --fd ]; then
        [ -e "/proc/self/fd/$argument" ] || exit 93
    fi
    previous=$argument
done
if [ "${FAKE_ASICEND_WAIT-0}" -eq 1 ]; then
    record_signal()
    {
        signal=$1
        if (set -C; : > "$FAKE_ASICEND_SIGNAL_LATCH") 2>/dev/null; then
            printf '%s\n' "$signal" > "$FAKE_ASICEND_SIGNAL_TMP"
            mv "$FAKE_ASICEND_SIGNAL_TMP" "$FAKE_ASICEND_SIGNAL"
        fi
    }
    stop()
    {
        signal=$1
        # The process-group signal can arrive again while the first trap is
        # publishing its latched result.  Do not re-enter the critical section.
        trap '' INT TERM HUP
        record_signal "$signal"
        if [ "${FAKE_ASICEND_STOP_DELAY-0}" != 0 ]; then
            sleep "$FAKE_ASICEND_STOP_DELAY"
        fi
        rm -f "$FAKE_ASICEND_ENDPOINT"
        exit $((128 + signal))
    }
    if [ "${FAKE_ASICEND_IGNORE_SIGNALS-0}" -eq 1 ]; then
        trap '' INT TERM HUP
    else
        trap 'stop 2' INT
        trap 'stop 15' TERM
        trap 'stop 1' HUP
    fi
    # Publish readiness only after all signal traps are installed.
    : > "$FAKE_ASICEND_ENDPOINT"
    if [ -n "${FAKE_ASICEND_READY-}" ]; then
        : > "$FAKE_ASICEND_READY"
    fi
    while :; do
        sleep 1
    done
fi
exit "${FAKE_ASICEND_EXIT:-0}"
EOF
chmod 0755 "$fake_bin/asicend"

usb_one=$test_root/usb-one
usb_two=$test_root/usb-two
firmware=$test_root/firmware\ with\ spaces.bin
runtime=$test_root/runtime\ with\ spaces
: > "$usb_one"
: > "$usb_two"
: > "$firmware"
log=$test_root/asicend.log
pid_file=$test_root/asicend.pid
termux_pid_dir=$test_root/termux-pids
callback_pid_dir=$test_root/callback-pids
endpoint=$test_root/fake-endpoint
signal_file=$test_root/asicend-signal
signal_latch=$test_root/asicend-signal.latch
signal_tmp=$test_root/asicend-signal.tmp
ready_file=$test_root/fake-ready
mkdir "$termux_pid_dir" "$callback_pid_dir"

PATH="$fake_bin:$PATH" FAKE_ASICEND_LOG="$log" FAKE_ASICEND_PID="$pid_file" \
    FAKE_TERMUX_PID_DIR="$termux_pid_dir" FAKE_CALLBACK_PID_DIR="$callback_pid_dir" \
    sh "$launcher" --usb-device "$usb_one" --usb-device "$usb_two" \
    --firmware "$firmware" --model w3u3 --instance termux.one \
    --runtime-dir "$runtime" --group --allow-lnb-power
assert_log_line '[--fd]' 'normal argument forwarding'
assert_log_line '[7]' 'normal first fd forwarding'
assert_log_line '[8]' 'normal second fd forwarding'
assert_log_line "[$firmware]" 'normal firmware forwarding'
assert_log_line '[--model]' 'normal model option forwarding'
assert_log_line '[w3u3]' 'normal model forwarding'
assert_log_line '[--instance]' 'normal instance option forwarding'
assert_log_line '[termux.one]' 'normal instance forwarding'
assert_log_line "[$runtime]" 'normal runtime directory forwarding'
assert_log_line '[--group]' 'normal group option forwarding'
assert_log_line '[--allow-lnb-power]' 'normal LNB option forwarding'
asicend_pid=$(cat "$pid_file")
if kill -0 "$asicend_pid" 2>/dev/null; then
    fail_test 'normal exit: asicend process remained after launcher exit'
fi

wait_for_file()
{
    file=$1
    attempts=0
    while [ ! -e "$file" ]; do
        attempts=$((attempts + 1))
        if [ "$attempts" -ge 20 ]; then
            printf 'asicen-termux test: state at timeout for %s:\n' "$file" >&2
            find "$test_root" -maxdepth 2 -print >&2
            ps -eo pid=,ppid=,pgid=,stat=,args= | awk -v root="$test_root" 'index($0, root) { print }' >&2
            fail_test "timed out waiting for $file"
        fi
        sleep 1
    done
}

wait_for_nonempty_file()
{
    file=$1
    attempts=0
    while [ ! -s "$file" ]; do
        attempts=$((attempts + 1))
        if [ "$attempts" -ge 20 ]; then
            printf 'asicen-termux test: state at timeout for non-empty %s:\n' "$file" >&2
            find "$test_root" -maxdepth 2 -print >&2
            ps -eo pid=,ppid=,pgid=,stat=,args= | awk -v root="$test_root" 'index($0, root) { print }' >&2
            fail_test "timed out waiting for non-empty $file"
        fi
        sleep 1
    done
}

process_group()
{
    awk '{print $5}' "/proc/$1/stat"
}

assert_pid_gone()
{
    pid=$1
    attempts=0
    while kill -0 "$pid" 2>/dev/null; do
        attempts=$((attempts + 1))
        [ "$attempts" -lt 20 ] || {
            fail_test "${context:-launcher signal}: process remained: $pid"
        }
        sleep 1
    done
}

launch_launcher()
{
    # POSIX shells ignore INT for asynchronous lists.  Reset it in an
    # external helper before setsid so the signal matrix reaches the launcher
    # as it would when invoked from an interactive Termux shell.
    perl -e '$SIG{INT} = "DEFAULT"; $SIG{TERM} = "DEFAULT"; $SIG{HUP} = "DEFAULT"; exec @ARGV or die "exec: $!"' \
        -- setsid sh "$launcher" "$@" &
    launcher_pid=$!
}

run_signal_test()
{
    signal_name=$1
    signal_number=$2
    expected_status=$((128 + signal_number))
    rm -f "$endpoint" "$signal_file" "$signal_latch" "$signal_tmp" \
        "$termux_pid_dir/one" "$termux_pid_dir/two" \
        "$callback_pid_dir/one" "$callback_pid_dir/two" "$pid_file"
    PATH="$fake_bin:$PATH" FAKE_ASICEND_LOG="$log" FAKE_ASICEND_PID="$pid_file" \
        FAKE_ASICEND_WAIT=1 FAKE_ASICEND_ENDPOINT="$endpoint" FAKE_ASICEND_SIGNAL="$signal_file" \
        FAKE_ASICEND_SIGNAL_LATCH="$signal_latch" FAKE_ASICEND_SIGNAL_TMP="$signal_tmp" \
        FAKE_TERMUX_PID_DIR="$termux_pid_dir" FAKE_CALLBACK_PID_DIR="$callback_pid_dir" \
        launch_launcher --usb-device "$usb_one" --usb-device "$usb_two" \
        --firmware "$firmware"
    wait_for_file "$endpoint"
    wait_for_nonempty_file "$termux_pid_dir/one"
    wait_for_nonempty_file "$termux_pid_dir/two"
    wait_for_nonempty_file "$callback_pid_dir/one"
    wait_for_nonempty_file "$callback_pid_dir/two"
    wait_for_nonempty_file "$pid_file"
    asicend_pid=$(cat "$pid_file")
    termux_pid_one=$(cat "$termux_pid_dir/one")
    termux_pid_two=$(cat "$termux_pid_dir/two")
    callback_pid_one=$(cat "$callback_pid_dir/one")
    callback_pid_two=$(cat "$callback_pid_dir/two")

    launcher_pgid=$(process_group "$launcher_pid")
    child_pgid=$(process_group "$termux_pid_one")
    [ -n "$launcher_pgid" ] || fail_test "$signal_name topology: launcher PGID missing"
    [ -n "$child_pgid" ] || fail_test "$signal_name topology: child PGID missing"
    [ "$child_pgid" != "$launcher_pgid" ] || fail_test "$signal_name topology: supervisor and child share PGID"
    for pid in "$termux_pid_two" "$callback_pid_one" "$callback_pid_two" "$asicend_pid"; do
        assert_equal "$child_pgid" "$(process_group "$pid")" "$signal_name topology for PID $pid"
    done

    kill "-$signal_name" "$launcher_pid"
    launcher_status=0
    wait "$launcher_pid" 2>/dev/null || launcher_status=$?
    assert_equal "$expected_status" "$launcher_status" "$signal_name launcher status"
    wait_for_nonempty_file "$signal_file"
    assert_equal "$signal_number" "$(cat "$signal_file")" "$signal_name asicend signal identity"
    for pid in "$asicend_pid" "$termux_pid_one" "$termux_pid_two" \
        "$callback_pid_one" "$callback_pid_two"; do
        context="$signal_name cleanup"
        assert_pid_gone "$pid"
    done
    assert_file_absent "$endpoint" "$signal_name endpoint cleanup"
    rm -f "$signal_latch" "$signal_tmp"
    assert_file_absent "$signal_latch" "$signal_name signal latch cleanup"
    assert_file_absent "$signal_tmp" "$signal_name signal temp cleanup"
}

run_signal_test INT 2
run_signal_test TERM 15
run_signal_test HUP 1

rm -f "$endpoint" "$signal_file" "$signal_latch" "$signal_tmp" \
    "$ready_file" "$termux_pid_dir/one" "$termux_pid_dir/two" \
    "$callback_pid_dir/one" "$callback_pid_dir/two" "$pid_file"
warning_file=$test_root/kill-warning
PATH="$fast_bin:$fake_bin:$PATH" FAKE_ASICEND_LOG="$log" FAKE_ASICEND_PID="$pid_file" \
    FAKE_ASICEND_WAIT=1 FAKE_ASICEND_IGNORE_SIGNALS=1 FAKE_ASICEND_ENDPOINT="$endpoint" \
    FAKE_ASICEND_SIGNAL="$signal_file" FAKE_ASICEND_SIGNAL_LATCH="$signal_latch" \
    FAKE_ASICEND_SIGNAL_TMP="$signal_tmp" FAKE_TERMUX_EXIT_ON_SIGNAL=1 \
    FAKE_TERMUX_PID_DIR="$termux_pid_dir" FAKE_CALLBACK_PID_DIR="$callback_pid_dir" \
    launch_launcher --usb-device "$usb_one" --usb-device "$usb_two" --firmware "$firmware" \
    2>"$warning_file"
wait_for_file "$endpoint"
wait_for_nonempty_file "$termux_pid_dir/one"
wait_for_nonempty_file "$termux_pid_dir/two"
wait_for_nonempty_file "$callback_pid_dir/one"
wait_for_nonempty_file "$callback_pid_dir/two"
wait_for_nonempty_file "$pid_file"
kill -TERM "$launcher_pid"
asicend_pid=$(cat "$pid_file")
termux_pid_one=$(cat "$termux_pid_dir/one")
termux_pid_two=$(cat "$termux_pid_dir/two")
callback_pid_one=$(cat "$callback_pid_dir/one")
callback_pid_two=$(cat "$callback_pid_dir/two")
launcher_status=0
wait "$launcher_pid" || launcher_status=$?
assert_equal 143 "$launcher_status" 'SIGKILL fallback direct child status'
grep -F 'using SIGKILL' "$warning_file" >/dev/null || fail_test 'SIGKILL fallback warning missing'
grep -F 'runtime endpoint removal are not guaranteed' "$warning_file" >/dev/null || \
    fail_test 'SIGKILL fallback cleanup warning incomplete'
assert_file_exists "$endpoint" 'SIGKILL fallback stale endpoint preservation'
for pid in "$asicend_pid" "$termux_pid_one" "$termux_pid_two" \
    "$callback_pid_one" "$callback_pid_two"; do
    context='SIGKILL fallback process cleanup'
    assert_pid_gone "$pid"
done
residue=$(ps -eo pid=,ppid=,pgid=,stat=,args= |
    awk -v root="$test_root" 'index($0, root) && $0 !~ /awk -v root=/ { print }')
[ -z "$residue" ] || fail_test "SIGKILL fallback process residue: $residue"
assert_file_absent "$signal_latch" 'SIGKILL fallback signal latch residue'
assert_file_absent "$signal_tmp" 'SIGKILL fallback signal temp residue'

rm -f "$endpoint" "$signal_file" "$signal_latch" "$signal_tmp" \
    "$ready_file" \
    "$termux_pid_dir/one" "$termux_pid_dir/two" \
    "$callback_pid_dir/one" "$callback_pid_dir/two" "$pid_file"
PATH="$fake_bin:$PATH" FAKE_ASICEND_LOG="$log" FAKE_ASICEND_PID="$pid_file" \
    FAKE_ASICEND_WAIT=1 FAKE_ASICEND_STOP_DELAY=0.5 FAKE_ASICEND_ENDPOINT="$endpoint" \
    FAKE_ASICEND_READY="$ready_file" \
    FAKE_ASICEND_SIGNAL="$signal_file" FAKE_ASICEND_SIGNAL_LATCH="$signal_latch" \
    FAKE_ASICEND_SIGNAL_TMP="$signal_tmp" FAKE_EARLY_EXIT_USB="$usb_one" \
    FAKE_TERMUX_PID_DIR="$termux_pid_dir" FAKE_CALLBACK_PID_DIR="$callback_pid_dir" \
    launch_launcher --usb-device "$usb_one" --usb-device "$usb_two" --firmware "$firmware"
wait_for_file "$ready_file"
wait_for_nonempty_file "$termux_pid_dir/one"
wait_for_nonempty_file "$termux_pid_dir/two"
wait_for_nonempty_file "$callback_pid_dir/one"
wait_for_nonempty_file "$callback_pid_dir/two"
wait_for_nonempty_file "$pid_file"
asicend_pid=$(cat "$pid_file")
termux_pid_one=$(cat "$termux_pid_dir/one")
termux_pid_two=$(cat "$termux_pid_dir/two")
callback_pid_one=$(cat "$callback_pid_dir/one")
callback_pid_two=$(cat "$callback_pid_dir/two")
launcher_status=0
wait "$launcher_pid" || launcher_status=$?
assert_equal 0 "$launcher_status" 'delayed normal cleanup launcher status'
wait_for_nonempty_file "$signal_file"
assert_equal 15 "$(cat "$signal_file")" 'delayed normal cleanup signal identity'
for pid in "$asicend_pid" "$termux_pid_one" "$termux_pid_two" \
    "$callback_pid_one" "$callback_pid_two"; do
    context='delayed normal cleanup'
    assert_pid_gone "$pid"
done
assert_file_absent "$endpoint" 'delayed normal cleanup endpoint'
rm -f "$signal_latch" "$signal_tmp"
assert_file_absent "$signal_latch" 'delayed normal cleanup signal latch'
assert_file_absent "$signal_tmp" 'delayed normal cleanup signal temp'

rm -f "$endpoint" "$termux_pid_dir/one" "$termux_pid_dir/two" \
    "$callback_pid_dir/one" "$callback_pid_dir/two" "$pid_file"
PATH="$fake_bin:$PATH" FAKE_FAIL_USB="$usb_two" \
    FAKE_TERMUX_PID_DIR="$termux_pid_dir" FAKE_CALLBACK_PID_DIR="$callback_pid_dir" \
    launch_launcher --usb-device "$usb_one" --usb-device "$usb_two" --firmware "$firmware"
wait_for_nonempty_file "$termux_pid_dir/one"
wait_for_nonempty_file "$termux_pid_dir/two"
wait_for_nonempty_file "$callback_pid_dir/one"
launcher_status=0
wait "$launcher_pid" || launcher_status=$?
assert_equal 17 "$launcher_status" 'second-open failure launcher status'
termux_pid_one=$(cat "$termux_pid_dir/one")
termux_pid_two=$(cat "$termux_pid_dir/two")
callback_pid_one=$(cat "$callback_pid_dir/one")
context='second-open failure cleanup'
assert_pid_gone "$termux_pid_one"
assert_pid_gone "$termux_pid_two"
assert_pid_gone "$callback_pid_one"
assert_file_absent "$pid_file" 'second-open failure asicend residue'
assert_file_absent "$endpoint" 'second-open failure endpoint residue'

PATH="$fake_bin:$PATH" sh "$launcher" --help >/dev/null
# A single-function ASICEN model opens one USB function. It opens only
# that device and passes exactly one --fd.
: > "$log"
PATH="$fake_bin:$PATH" FAKE_ASICEND_LOG="$log" FAKE_ASICEND_PID="$pid_file" \
    FAKE_TERMUX_PID_DIR="$termux_pid_dir" \
    sh "$launcher" --usb-device "$usb_one" --firmware "$firmware" --model s3u
assert_equal 6 "$(head -n 1 "$log")" 'single-device argument count'
assert_log_line '[--fd]' 'single-device fd option'
assert_log_line '[7]' 'single-device fd forwarding'
assert_log_line '[s3u]' 'single-device model forwarding'
if grep -Fx '[8]' "$log" >/dev/null; then
    fail_test 'single-device path opened a second device'
fi
if PATH="$fake_bin:$PATH" sh "$launcher" --usb-device "$usb_one" --usb-device "$usb_two" \
    --usb-device "$usb_one" --firmware "$firmware"; then
    exit 1
fi
if PATH="$fake_bin:$PATH" sh "$launcher" --firmware "$firmware"; then
    exit 1
fi
if PATH="$fake_bin:$PATH" sh "$launcher" --usb-device "$usb_one" --usb-device "$usb_one" \
    --firmware "$firmware"; then
    exit 1
fi
if PATH="$fake_bin:$PATH" sh "$launcher" --usb-device "$usb_one" --usb-device "$usb_two" \
    --firmware "$firmware" --fd 7; then
    exit 1
fi
if PATH="$fake_bin:$PATH" sh "$launcher" --usb-device "$usb_one" --usb-device "$usb_two" \
    --firmware "$firmware" --unknown; then
    exit 1
fi

exec 7<"$usb_one"
if PATH="$fake_bin:$PATH" ASICEN_TERMUX_STAGE=2 ASICEN_TERMUX_FD1=7 sh "$launcher" 7; then
    exit 1
fi
if PATH="$fake_bin:$PATH" ASICEN_TERMUX_STAGE=2 ASICEN_TERMUX_FD1=999999 sh "$launcher" 8; then
    exit 1
fi
exec 8<"$usb_one"
if PATH="$fake_bin:$PATH" ASICEN_TERMUX_STAGE=2 ASICEN_TERMUX_FD1=7 sh "$launcher" 8; then
    exit 1
fi
exec 8<&-
exec 7<&-

if PATH="$fake_bin:$PATH" FAKE_FAIL_USB="$usb_two" \
    sh "$launcher" --usb-device "$usb_one" --usb-device "$usb_two" --firmware "$firmware"; then
    exit 1
else
    test "$?" -eq 17
fi

: > "$log"
if PATH="$fake_bin:$PATH" FAKE_ASICEND_LOG="$log" FAKE_ASICEND_PID="$pid_file" FAKE_ASICEND_EXIT=23 \
    sh "$launcher" --usb-device "$usb_one" --usb-device "$usb_two" --firmware "$firmware"; then
    exit 1
else
    test "$?" -eq 23
fi

# ASICEN devices may already be in runtime state; firmware is optional.
PATH="$fake_bin:$PATH" FAKE_ASICEND_LOG="$log" FAKE_ASICEND_PID="$pid_file" \
    sh "$launcher" --usb-device "$usb_one"
assert_equal '2' "$(head -n 1 "$log")" 'runtime single-function argv count'
assert_log_line '[--fd]' 'runtime fd option'
assert_log_line '[7]' 'runtime fd value'
if PATH="$fake_bin:$PATH" sh "$launcher" --usb-device "$usb_one" --model w3u3 --model s3u; then
    fail_test 'duplicate model accepted'
fi
if PATH="$fake_bin:$PATH" sh "$launcher" --help --usb-device "$usb_one"; then
    fail_test 'help combined with arguments accepted'
fi

printf '%s\n' 'asicen-termux offline tests: PASS'
