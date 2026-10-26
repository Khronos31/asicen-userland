#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Collect an ASICEN report. Paste the whole stdout into a GitHub issue.
# USB enumeration is read-only. Set ASICEN_REPORT_USB_PATH and
# ASICEN_REPORT_MODEL to select an enclosure; paired models also require
# ASICEN_REPORT_USB_PATH2. An explicitly selected daemon session can initialize
# hardware and tune unless ASICEN_REPORT_TUNE=0. Satellite requests use 0 V;
# models without software LNB switching cannot guarantee physical power-off.
set -eu

section() {
    printf '\n## %s\n' "$1"
}

note() {
    printf '%s\n' "$1"
}

have() {
    command -v "$1" >/dev/null 2>&1
}

bindir=${ASICEN_BINDIR:-}
find_bin() {
    name=$1
    if [ -n "$bindir" ] && [ -x "$bindir/$name" ]; then
        printf '%s\n' "$bindir/$name"
        return 0
    fi
    if have "$name"; then
        command -v "$name"
        return 0
    fi
    return 1
}

firmware=
if [ -n "${ASICEN_FIRMWARE:-}" ]; then
    firmware=$ASICEN_FIRMWARE
else
    for candidate in ./asicen-loader.bin \
        /usr/local/share/asicen-userland/asicen-loader.bin \
        /usr/share/asicen-userland/asicen-loader.bin
    do
        if [ -f "$candidate" ]; then
            firmware=$candidate
            break
        fi
    done
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/asicen-report.XXXXXX")
chmod 700 "$work"
runtime=
asicend_pid=
# Called from the EXIT trap.
# shellcheck disable=SC2329
cleanup() {
    if [ -n "$asicend_pid" ]; then
        kill -INT "$asicend_pid" 2>/dev/null || true
        wait "$asicend_pid" 2>/dev/null || true
        asicend_pid=
    fi
    if [ -n "$runtime" ]; then
        rm -rf "$runtime"
        runtime=
    fi
    rm -rf "$work"
}
trap cleanup EXIT INT TERM

printf '%s\n' 'asicen-userland ASICEN report'
printf '%s\n' 'Paste this whole log into a GitHub issue. It does not include the firmware file.'
date -Is 2>/dev/null || date

section 'host'
uname -srm || true
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    printf 'os=%s %s\n' "${NAME:-unknown}" "${VERSION:-}"
fi
id || true

section 'usb'
if have lsusb; then
    lsusb -d 0b06: || note 'lsusb: no 0b06 devices'
    lsusb -d 1738: || note 'lsusb: no 1738 devices'
else
    note 'lsusb: not installed'
fi

asicen_devices=
sysfs=${ASICEN_REPORT_SYSFS:-/sys/bus/usb/devices}
if [ -d "$sysfs" ]; then
    for device in "$sysfs"/*; do
        [ -r "$device/idVendor" ] || continue
        vendor=$(cat "$device/idVendor")
        case "$vendor" in 0b06|1738) ;; *) continue ;; esac
        product=$(cat "$device/idProduct" 2>/dev/null || printf '%s' '?')
        serial=$(cat "$device/serial" 2>/dev/null || printf '%s' '?')
        speed=$(cat "$device/speed" 2>/dev/null || printf '%s' '?')
        printf 'sysfs %s id=%s:%s serial=%s speed=%s\n' \
            "$(basename "$device")" "$vendor" "$product" "$serial" "$speed"
        case "$vendor:$product" in
            0b06:0001|0b06:0003|0b06:0004|0b06:0005|0b06:0006|1738:5211|1738:5216)
                asicen_devices="$asicen_devices $(basename "$device")" ;;
        esac
    done
else
    note 'sysfs: /sys/bus/usb/devices is not available'
fi

section 'kernel'
if dmesg -T >"$work/dmesg.txt" 2>"$work/dmesg.err"; then
    if grep -E '0b06:|1738:|asicen|usb [0-9]+-[0-9]' "$work/dmesg.txt" | tail -n 40; then
        :
    else
        note 'dmesg: no recent USB lines matched'
    fi
else
    note 'dmesg: not readable'
    cat "$work/dmesg.err" || true
fi
rm -f "$work/dmesg.txt" "$work/dmesg.err"

section 'tools'
asicend=$(find_bin asicend || true)
asicenctl=$(find_bin asicenctl || true)
asicents=$(find_bin asicen-ts || true)
printf 'asicend=%s\n' "${asicend:-missing}"
printf 'asicenctl=%s\n' "${asicenctl:-missing}"
printf 'asicen-ts=%s\n' "${asicents:-missing}"
printf 'firmware=%s\n' "${firmware:-missing}"
if [ -n "$asicend" ]; then
    "$asicend" --help >/dev/null || note 'asicend --help failed'
fi

if [ -z "$asicen_devices" ]; then
    section 'result'
    note 'No supported ASICEN runtime or loader was visible. Plug it in directly and run this script again.'
    exit 1
fi

usb_path=${ASICEN_REPORT_USB_PATH:-}
usb_path2=${ASICEN_REPORT_USB_PATH2:-}
model=${ASICEN_REPORT_MODEL:-}
if [ -z "$asicend" ] || [ -z "$asicenctl" ] || [ -z "$usb_path" ] || [ -z "$model" ]; then
    section 'result'
    note 'USB identity was collected. The daemon session needs asicend, asicenctl, and an explicit topology/model selection.'
    note 'Set ASICEN_BINDIR, ASICEN_REPORT_USB_PATH, ASICEN_REPORT_MODEL, and ASICEN_REPORT_USB_PATH2 for paired models.'
    note 'Set ASICEN_FIRMWARE when a loader image is needed, then run again.'
    exit 0
fi
case "$model" in
    s3u) terrestrial_receiver=0; [ -z "$usb_path2" ] || exit 2 ;;
    s3u2) terrestrial_receiver=1; [ -z "$usb_path2" ] || exit 2 ;;
    w3u2|w3u3|w3u3-v2) terrestrial_receiver=1; [ -n "$usb_path2" ] || {
        note 'Paired ASICEN models require ASICEN_REPORT_USB_PATH2.'; exit 2;
    } ;;
    *) note 'ASICEN_REPORT_MODEL is not a supported model key.'; exit 2 ;;
esac

instance=report
section "asicend $model $usb_path ${usb_path2:-}"
runtime=$(mktemp -d "${TMPDIR:-/tmp}/asicenrt.XXXXXX")
# Leave room for the product/instance suffix and socket name on macOS as well
# as Linux. A long per-user TMPDIR must not make the report impossible to run.
if [ "$(printf '%s' "$runtime" | LC_ALL=C wc -c)" -gt 64 ]; then
    rmdir "$runtime"
    runtime=$(mktemp -d /tmp/asicenrt.XXXXXX)
fi
chmod 700 "$runtime"
log=$runtime/asicend.log
set -- "$asicend" --usb-path "$usb_path"
if [ -n "$usb_path2" ]; then set -- "$@" --usb-path "$usb_path2"; fi
set -- "$@" --model "$model" --instance "$instance" --runtime-dir "$runtime"
if [ -n "$firmware" ]; then set -- "$@" --firmware "$firmware"; fi
"$@" >"$log" 2>&1 &
asicend_pid=$!
ready=0
i=0
while [ "$i" -lt 90 ]; do
    if grep -q 'asicend ready' "$log"; then
        ready=1
        break
    fi
    if ! kill -0 "$asicend_pid" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 1
done
note "ready=$ready"
cat "$log" || true
if [ "$ready" -eq 1 ]; then
    note '--- asicenctl list ---'
    "$asicenctl" --instance "$instance" --runtime-dir "$runtime" list || true
    note '--- asicenctl status ---'
    "$asicenctl" --instance "$instance" --runtime-dir "$runtime" status || true
    note '--- card ---'
    "$asicenctl" --instance "$instance" --runtime-dir "$runtime" card-status || true
    "$asicenctl" --instance "$instance" --runtime-dir "$runtime" card-atr || true
    if [ -n "$asicents" ] && [ "${ASICEN_REPORT_TUNE:-1}" != 0 ]; then
        note "--- terrestrial receiver $terrestrial_receiver, 5s, 527143 kHz ---"
        "$asicents" --instance "$instance" --runtime-dir "$runtime" \
            --receiver "$terrestrial_receiver" --system isdb-t --frequency-khz 527143 \
            --duration-seconds 5 --output /dev/null || true
        note '--- satellite receiver 0, 5s, 1318000 kHz, slot 0, LNB 0V ---'
        "$asicents" --instance "$instance" --runtime-dir "$runtime" \
            --receiver 0 --system isdb-s --frequency-khz 1318000 \
            --slot 0 --lnb-voltage 0 --duration-seconds 5 --output /dev/null || true
        note '--- asicenctl status after tune ---'
        "$asicenctl" --instance "$instance" --runtime-dir "$runtime" status || true
    fi
fi
kill -INT "$asicend_pid" 2>/dev/null || true
wait "$asicend_pid" 2>/dev/null || true
asicend_pid=
rm -rf "$runtime"
runtime=

section 'result'
note 'Finished. Paste this log into the issue. A tune timeout usually means the antenna was not connected.'
exit 0
