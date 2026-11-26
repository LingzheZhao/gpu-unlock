#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Exercise CMP 170HX dynamic FBP range sizing and atomic Gen4 E1 PHY writes.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
sed -n '/^int gu_memory_decode_range/,/^static int memory_read/p' \
	"$repo/kernel/gpu_unlock/memory.c" | sed '$d' > "$tmp/ga100-mem.c"
{
	sed -n '/^static bool pcie_reg_readable/,/^}/p' "$repo/kernel/gpu_unlock/pcie.c"
	sed -n '/^static int ga100_apply_gen3_phy/,/^}/p' "$repo/kernel/gpu_unlock/pcie.c"
	sed -n '/^static int ga100_apply_gen4_e1/,/^}/p' "$repo/kernel/gpu_unlock/pcie.c"
} > "$tmp/ga100-e1.c"
sed -n '/^static const char gu_fallback_full/,/^#if defined(CONFIG_KRETPROBES)/p' \
	"$repo/kernel/gpu_unlock/name.c" | sed '$d' > "$tmp/ga100-name.c"
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -I"$tmp" -I"$repo/kernel/gpu_unlock" \
	"$repo/tests/pcie/ga100.c" -o "$tmp/ga100"
"$tmp/ga100"
