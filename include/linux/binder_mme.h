/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BINDER_MME_H
#define _LINUX_BINDER_MME_H
struct file;
struct task_struct;
/* Private CAP_SYS_ADMIN C06 bridge; returns a task ref or ERR_PTR. */
struct task_struct *binder_mme_get_task(struct file *file, unsigned int handle);
#endif
