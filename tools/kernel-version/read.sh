#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Run on the target Android host, not in a Linux container.
node=/proc/chiruno_kernel_version
# A sentinel preserves every trailing LF through command substitution.
record=$(cat "$node" && printf '.') || {
	printf '%s\n' "$node: not enabled or inaccessible" >&2
	exit 1
}
release=$(uname -r) || exit 1
if [ -z "$release" ] || [ "$record" != "$release
." ]; then
	printf '%s\n' "$node: expected exactly uname -r followed by one LF" >&2
	exit 1
fi
printf '%s: %s\n' "$node" "$release"
