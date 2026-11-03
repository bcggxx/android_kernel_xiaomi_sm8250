/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MI_FAULT_AROUND_H
#define _LINUX_MI_FAULT_AROUND_H

#include <linux/types.h>

#ifdef CONFIG_MI_FAULT_AROUND
bool mi_fault_around_allowed(void);
#else
static inline bool mi_fault_around_allowed(void)
{
	return true;
}
#endif
#endif
