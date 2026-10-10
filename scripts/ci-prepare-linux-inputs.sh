#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Compatibility name; arguments and output follow package-source.sh.
set -eu
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
exec "$script_dir/package-source.sh" "$@"
