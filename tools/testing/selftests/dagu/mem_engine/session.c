// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include "../../../../../mm/mi_mem_engine_policy.h"
int main(void)
{
	struct mme_request r = { .version = MME_VERSION, .size = sizeof(r),
		.cookie = 2, .lease_ms = 1000, .nr_tasks = 1,
		.tasks = {{ .binder_fd = 3, .binder_handle = 1, .tid = 42, .uid = 10001 }} };
	assert(mme_validate_begin(&r, 1) == 0);
	r.cookie = 1;
	assert(mme_validate_begin(&r, 1) == -ESTALE);
	r.cookie = 2; r.lease_ms = 1001;
	assert(mme_validate_begin(&r, 1) == -EINVAL);
	r.lease_ms = 1000; r.tasks[1].uid = 1;
	assert(mme_validate_begin(&r, 1) == -EINVAL);
	assert(mme_policy_mark(101, 1) == 76);
	assert(mme_policy_mark(101, 0) == 101);
	assert(!mme_session_live(100, 100));
	assert(mme_session_live(99, 100));
	assert(!mme_end_matches(1, 2));
	assert(mme_end_matches(2, 2));
	return 0;
}
