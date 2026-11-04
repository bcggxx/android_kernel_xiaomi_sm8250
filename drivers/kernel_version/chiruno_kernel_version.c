// SPDX-License-Identifier: GPL-2.0
/* Read-only identity of this target kernel build. */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <generated/utsrelease.h>

static struct proc_dir_entry *version_entry;

static int version_show(struct seq_file *seq, void *unused)
{
	seq_puts(seq, UTS_RELEASE "\n");
	return 0;
}

static int version_open(struct inode *inode, struct file *file)
{
	return single_open(file, version_show, NULL);
}

static const struct file_operations version_fops = {
	.owner = THIS_MODULE,
	.open = version_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init version_init(void)
{
	version_entry = proc_create("chiruno_kernel_version", 0444, NULL,
				    &version_fops);
	return version_entry ? 0 : -ENOMEM;
}

static void __exit version_exit(void)
{
	proc_remove(version_entry);
}

module_init(version_init);
module_exit(version_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only target kernel release metadata");
