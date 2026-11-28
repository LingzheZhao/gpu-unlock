#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Exercise GA100 HBM governor busy-NDIV override and FBPA0-fused probing.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
sed -E '/^#include <linux\//d' "$repo/kernel/gpu_unlock/hbm.h" > "$tmp/hbm.h"
sed -E '/^#include <linux\//d' "$repo/kernel/gpu_unlock/hbm.c" > "$tmp/hbm-unit.c"
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -I"$tmp" -I"$repo/kernel/gpu_unlock" \
	"$repo/tests/hbm/busy_ndiv.c" -o "$tmp/busy_ndiv"
"$tmp/busy_ndiv"
