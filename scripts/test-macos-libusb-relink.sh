#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Compatibility name; arguments and output follow test-static-relink.sh.
set -eu
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
exec "$script_dir/test-static-relink.sh" "$@"
