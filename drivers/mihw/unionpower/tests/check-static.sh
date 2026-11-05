#!/bin/sh
set -eu

dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
repo="$(CDPATH= cd -- "$dir/../../.." && pwd)"

check() {
	name="$1"
	shift
	"$@" >/dev/null
	printf 'PASS %s\n' "$name"
}

check "native package fields" \
	grep -q 'struct timespec64 current_timespec' "$dir/unionpower_uapi.h"
check "fixed ioctl commands" \
	grep -q "_IOW('g', 1, struct _BUFFER_PACKAGE)" "$dir/unionpower_uapi.h"
check "fixed vsync command" \
	grep -q "_IOW('g', 5, struct _BUFFER_PACKAGE)" "$dir/unionpower_uapi.h"
check "48-byte compile assertion" \
	grep -q 'sizeof(struct _BUFFER_PACKAGE) != 48' "$dir/unionpower_core.c"
check "4.19 file_operations mapping" \
	grep -q '\.open = unionpower_ioctl_open' "$dir/union_power_ioctl.c"
check "compat returns ENOTTY" \
	grep -q 'return -ENOTTY' "$dir/union_power_ioctl.c"
check "monitor disabled by default" \
	grep -q '^int FRAME_MONITOR_ENABLE;' "$dir/union_power_ioctl.c"
check "monitor exact-enable check" \
	grep -q 'FRAME_MONITOR_ENABLE) != 1' "$dir/union_power_ioctl.c"
check "exact RenderThread check" \
	grep -q '!strcmp(comm, "RenderThread")' "$dir/unionpower_core.c"
check "five fixed record slots" \
	grep -q 'UNIONPOWER_RECORD_SIZE.*5' "$dir/unionpower.h"
check "64 work limit" \
	grep -q 'UNIONPOWER_MAX_WORK.*64' "$dir/unionpower.h"
check "64 record limit" \
	grep -q 'UNIONPOWER_MAX_RECORDS.*64' "$dir/unionpower.h"
check "ordered workqueue" \
	grep -q 'alloc_ordered_workqueue' "$dir/unionpower_core.c"
check "queue_work result checked" \
	grep -q '!queue_work(queue_buffer_wq' "$dir/unionpower_core.c"
check "pending reserved before queue_work" \
	sh -c "reserve=\$(grep -n -m1 'pending_work_count++;' '$dir/unionpower_core.c' | cut -d: -f1); queue=\$(grep -n -m1 '!queue_work(queue_buffer_wq' '$dir/unionpower_core.c' | cut -d: -f1); test \"\$reserve\" -lt \"\$queue\""
check "one-second cleanup" \
	grep -q '&cleanup_work, HZ' "$dir/unionpower_core.c"
check "cleanup recalculates after record removal" \
	sh -c "sed -n '/static void unionpower_cleanup_records_locked/,/^}/p' '$dir/unionpower_core.c' | grep -q 'unionpower_recalculate_frame_jank_locked(ktime_to_us(ktime_get()))'"
check "cleanup requeue ignores mod_delayed_work result" \
	sh -c "sed -n '/void unionpower_cleanup_work_fn/,/^}/p' '$dir/unionpower_core.c' | grep -q 'mod_delayed_work(queue_buffer_wq, &cleanup_work, HZ);'"
check "cleanup requeue is not a queue failure" \
	sh -c "! sed -n '/void unionpower_cleanup_work_fn/,/^}/p' '$dir/unionpower_core.c' | grep -q 'queue_failure'"
check "writer pointer cleared on stop" \
	grep -q 'WRITE_ONCE(unionpower_writer, NULL)' "$dir/unionpower_core.c"
check "workqueue destroyed last" \
	grep -q 'unionpower_core_destroy();' "$dir/unionpower_main.c"
check "global jank READ_ONCE" \
	grep -q 'READ_ONCE(frame_jank)' "$dir/unionpower_core.c"
check "global jank WRITE_ONCE" \
	grep -q 'WRITE_ONCE(frame_jank' "$dir/unionpower_core.c"
check "lookup record host-only" \
	sh -c "grep -B2 '^unionpower_lookup_record' '$dir/unionpower.h' | grep -q '#ifdef UNIONPOWER_HOST_TEST'"
check "stats snapshot locked" \
	sh -c "sed -n '/struct unionpower_stats unionpower_get_stats/,/^}/p' '$dir/unionpower_core.c' | grep -q 'spin_lock_irqsave'"
check "pending snapshot locked" \
	sh -c "sed -n '/int unionpower_pending_work_count/,/^}/p' '$dir/unionpower_core.c' | grep -q 'spin_lock_irqsave'"
check "Kconfig default n" \
	sh -c "grep -A3 '^config MI_UNION_POWER' '$repo/drivers/mihw/Kconfig' | grep -q 'default n'"
check "reserved start not consumed" \
	sh -c "! grep -Eq 'package[.]start|package[.]frame_time' '$dir/union_power_ioctl.c'"
check "no fpsChange empty path" \
	sh -c "! grep -Rq --exclude-dir=tests 'fpsChange\\|target_fps' '$dir'"
check "no CPU governor policy" \
	sh -c "! grep -Rq --exclude-dir=tests 'cpufreq\\|governor' '$dir'"
