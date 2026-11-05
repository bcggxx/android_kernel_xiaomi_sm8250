#!/bin/sh
set -eu

test_name="${1:-cycle1}"
cc="${CC:-gcc}"
dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
out="${TMPDIR:-/tmp}/unionpower-${test_name}"

"$cc" -std=gnu11 -Wall -Werror -Wno-unused-parameter \
	-Wno-unused-variable -Wno-unused-function -DUNIONPOWER_HOST_TEST \
	-DCONFIG_COMPAT \
	-I"$dir" -I"$dir/.." \
	"$dir/../unionpower_core.c" \
	"$dir/../union_power_ioctl.c" \
	"$dir/host_support.c" \
	"$dir/test_${test_name}.c" \
	-o "$out"
"$out"
