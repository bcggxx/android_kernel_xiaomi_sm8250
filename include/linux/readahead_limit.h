/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_READAHEAD_LIMIT_H
#define _LINUX_READAHEAD_LIMIT_H
#include <linux/types.h>
struct readahead_limit_sample {
	unsigned int source;
	unsigned long demand;
	unsigned long demand_size;
	unsigned long raw_start;
	unsigned long raw_size;
	unsigned long raw_async;
	unsigned long final_start;
	unsigned long final_size;
	unsigned long final_async;
	bool force;
	int reason;
	u64 identity;
	u64 target_identity;
	long available;
	u64 generation;
};
#endif
