// SPDX-License-Identifier: GPL-2.0
/* Independent UID policy adaptation; see Documentation/vm/mi-fault-around.rst. */
#include <linux/bitmap.h>
#include <linux/cred.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/mi_fault_around.h>

#define MI_FAULT_UID_BASE 10000U
#define MI_FAULT_BITS 512U

static bool enabled;
static DECLARE_BITMAP(fault_around, MI_FAULT_BITS);
static DEFINE_MUTEX(fault_around_mutex);

static int fault_around_set(const char *val, const struct kernel_param *kp)
{
	unsigned int value, index;
	int ret;

	ret = kstrtouint(val, 10, &value);
	if (ret)
		return ret;
	if (value && value < MI_FAULT_UID_BASE)
		return -EINVAL;
	index = value % MI_FAULT_UID_BASE;
	if (index >= MI_FAULT_BITS)
		return -ENOSPC;

	mutex_lock(&fault_around_mutex);
	if (!value) {
		/* Readers can observe intermediate individual-bit states. */
		for (index = 0; index < MI_FAULT_BITS; index++)
			clear_bit(index, fault_around);
	} else {
		set_bit(index, fault_around);
	}
	mutex_unlock(&fault_around_mutex);
	return 0;
}

static int fault_around_get(char *buffer, const struct kernel_param *kp)
{
	unsigned int index;
	int len = 0;

	mutex_lock(&fault_around_mutex);
	for (index = 0; index < MI_FAULT_BITS; index++) {
		if (test_bit(index, fault_around))
			len += scnprintf(buffer + len, PAGE_SIZE - len, "%s%u",
					 len ? " " : "", MI_FAULT_UID_BASE + index);
	}
	len += scnprintf(buffer + len, PAGE_SIZE - len, "\n");
	mutex_unlock(&fault_around_mutex);
	return len;
}

static const struct kernel_param_ops fault_around_ops = {
	.set = fault_around_set,
	.get = fault_around_get,
};
module_param(enabled, bool, 0600);
MODULE_PARM_DESC(enabled, "Enable UID fault-around selection (default false)");
module_param_cb(fault_around, &fault_around_ops, NULL, 0600);
MODULE_PARM_DESC(fault_around, "UID remainder deny bitmap: 0 clears, others add");

bool mi_fault_around_allowed(void)
{
	u32 uid;
	unsigned int index;

	if (!READ_ONCE(enabled))
		return true;
	uid = __kuid_val(current_uid());
	if (uid < MI_FAULT_UID_BASE)
		return false;
	index = uid % MI_FAULT_UID_BASE;
	return index >= MI_FAULT_BITS || !test_bit(index, fault_around);
}
