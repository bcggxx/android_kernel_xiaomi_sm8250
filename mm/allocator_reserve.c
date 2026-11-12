// SPDX-License-Identifier: GPL-2.0
#include <linux/mi_mempool.h>
#include <linux/mutex.h>

static DEFINE_MUTEX(reserve_owner_lock);
static enum allocator_reserve_owner reserve_owner;

int allocator_reserve_acquire(enum allocator_reserve_owner owner)
{
	int ret = 0;

	if (owner < RESERVE_NONE || owner > RESERVE_C22)
		return -EINVAL;
	mutex_lock(&reserve_owner_lock);
	if (owner == RESERVE_NONE || reserve_owner != RESERVE_NONE)
		ret = -EBUSY;
	else
		reserve_owner = owner;
	mutex_unlock(&reserve_owner_lock);
	return ret;
}

void allocator_reserve_release(enum allocator_reserve_owner owner)
{
	mutex_lock(&reserve_owner_lock);
	if (WARN_ON(reserve_owner != owner || owner == RESERVE_NONE))
		goto out;
	reserve_owner = RESERVE_NONE;
out:
	mutex_unlock(&reserve_owner_lock);
}
