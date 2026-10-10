#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$script_dir/audit-artifact.py" "$@"
