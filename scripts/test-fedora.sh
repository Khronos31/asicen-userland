#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Offline/static checks for the Fedora SELinux, systemd, sysusers, and Polkit
# integration. Fedora-only compilation and hardware checks run separately.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
fedora="$root/packaging/fedora"
policy="$fedora/selinux/asicend.te"
file_contexts="$fedora/selinux/asicend.fc"
unit="$fedora/systemd/asicend@.service"
sysusers="$fedora/sysusers.d/asicen-userland.conf"
polkit="$fedora/polkit/50-asicen-userland.rules"

for path in "$policy" "$file_contexts" "$unit" "$sysusers" "$polkit"; do
    test -f "$path"
    grep -F 'SPDX-License-Identifier: GPL-2.0-only' "$path" >/dev/null
done

sh -n "$0"
shellcheck -S error "$0"

grep -F 'policy_module(asicend, 1.0.0)' "$policy" >/dev/null
grep -F 'type asicend_t;' "$policy" >/dev/null
grep -F 'type asicend_exec_t;' "$policy" >/dev/null
grep -F 'type asicend_var_run_t;' "$policy" >/dev/null
grep -F 'type asicend_data_t;' "$policy" >/dev/null
grep -F 'files_pid_file(asicend_var_run_t)' "$policy" >/dev/null
grep -F 'files_pid_filetrans(init_t, asicend_var_run_t, dir, "asicen-userland")' "$policy" >/dev/null
grep -F 'init_daemon_domain(asicend_t, asicend_exec_t)' "$policy" >/dev/null
grep -F 'init_nnp_daemon_domain(asicend_t)' "$policy" >/dev/null
if grep -F 'allow asicend_t device_t:dir { getattr open read search };' "$policy" >/dev/null; then
    printf '%s\n' 'Fedora SELinux policy must not grant speculative device_t directory access' >&2
    exit 1
fi
grep -F 'dev_rw_generic_usb_dev(asicend_t)' "$policy" >/dev/null
grep -F 'dev_read_sysfs(asicend_t)' "$policy" >/dev/null
grep -F 'udev_read_db(asicend_t)' "$policy" >/dev/null
grep -F 'allow asicend_t self:netlink_kobject_uevent_socket create_socket_perms;' "$policy" >/dev/null
grep -F 'files_search_usr(asicend_t)' "$policy" >/dev/null
grep -F 'read_files_pattern(asicend_t, asicend_data_t, asicend_data_t)' "$policy" >/dev/null
grep -F 'stream_connect_pattern(pcscd_t, asicend_var_run_t, asicend_var_run_t, asicend_t)' "$policy" >/dev/null

# Keep the pcscd rule auditable: no wildcard target, broad domain, or write
# capability may be hidden in a second direct allow statement.
pcscd_allows=$(grep -E '^[[:space:]]*allow[[:space:]]+pcscd_t[[:space:]]' "$policy" || true)
test -z "$pcscd_allows"
test "$(printf '%s\n' "$pcscd_allows" | grep -Ec 'unconfined|self:|domain|\*' || true)" -eq 0

grep -F '/opt/asicen-userland/asicend -- gen_context(system_u:object_r:asicend_exec_t,s0)' "$file_contexts" >/dev/null
grep -F '/opt/asicen-userland/firmware/asicen-loader\.bin -- gen_context(system_u:object_r:asicend_data_t,s0)' "$file_contexts" >/dev/null
grep -F '/run/asicen-userland(/.*)? gen_context(system_u:object_r:asicend_var_run_t,s0)' "$file_contexts" >/dev/null

grep -F 'User=asicend' "$unit" >/dev/null
grep -F 'Group=pcscd' "$unit" >/dev/null
grep -F 'SupplementaryGroups=video' "$unit" >/dev/null
grep -F 'RuntimeDirectory=asicen-userland' "$unit" >/dev/null
grep -F 'RuntimeDirectoryMode=0750' "$unit" >/dev/null
grep -F 'UMask=0007' "$unit" >/dev/null
grep -F 'EnvironmentFile=/etc/asicen-userland/%i.conf' "$unit" >/dev/null
# shellcheck disable=SC2016
grep -F 'asicend $ASICEN_DEVICE_ARGUMENTS --instance %i' "$unit" >/dev/null
grep -F -- '--firmware /opt/asicen-userland/firmware/asicen-loader.bin' "$unit" >/dev/null
grep -F -- '--runtime-dir /run/asicen-userland --group' "$unit" >/dev/null
# SocketListener validates the explicit runtime base as daemon-owned with mode
# 0750.  The daemon then appends asicen-userland/<instance>, so this unit's actual
# socket hierarchy is /run/asicen-userland/asicen-userland/<instance>/.  Keep the
# dedicated base and group-mode socket permissions aligned with that contract
# (0750 directories and 0660 sockets via UMask=0007).
grep -F 'ReadWritePaths=/run/asicen-userland' "$unit" >/dev/null
grep -F 'Restart=no' "$unit" >/dev/null
grep -F 'NoNewPrivileges=yes' "$unit" >/dev/null
if grep -F 'does not opt into the separate NNP transition path' "$unit" >/dev/null; then
    printf '%s\n' 'systemd unit must not claim that the policy omits the NNP transition' >&2
    exit 1
fi
if grep -F 'Restart=on-failure' "$unit" >/dev/null; then
    printf '%s\n' 'systemd unit must not restart indefinitely after hardware failure' >&2
    exit 1
fi
grep -F 'PrivateDevices' "$unit" | grep -Fv 'Do not use' >/dev/null && {
    printf '%s\n' 'systemd unit must not enable PrivateDevices' >&2
    exit 1
}
grep -F 'CapabilityBoundingSet=' "$unit" >/dev/null

awk '
    /^[[:space:]]*(#|$)/ { next }
    count == 0 && $1 == "u" && $2 == "asicend" && $3 == "-" { count++; next }
    count == 1 && $1 == "m" && $2 == "asicend" && $3 == "pcscd" { count++; next }
    { exit 1 }
    END { exit count == 2 ? 0 : 1 }
' "$sysusers"

grep -F 'action.id === "org.debian.pcsc-lite.access_pcsc"' "$polkit" >/dev/null
grep -F 'action.id === "org.debian.pcsc-lite.access_card"' "$polkit" >/dev/null
grep -F 'subject.isInGroup("pcscd")' "$polkit" >/dev/null
if grep -E 'isInGroup\("(wheel|video|users)"\)|action\.id[^=]*==[^=]*\*' "$polkit" >/dev/null; then
    printf '%s\n' 'Polkit rule contains an out-of-scope group or wildcard action' >&2
    exit 1
fi

printf '%s\n' 'Fedora integration static checks: PASS'
