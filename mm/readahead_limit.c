// SPDX-License-Identifier: GPL-2.0
/* Experimental raw candidate budget; EOF remains the submitter's concern. */
#include <linux/cgroup.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/cpuset.h>
#include <linux/file.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/pagemap.h>
#include <trace/events/filemap.h>
#include "internal.h"

/* -1: unprovable range, 0: unchanged, 1: strictly clipped. */
static int ra_limit_range(pgoff_t *start, unsigned long *size,
		unsigned long *async, pgoff_t demand, unsigned long demand_size)
{
	pgoff_t end, demand_end, left, right;
	unsigned long extra;

	if (*async > *size || *size > ULONG_MAX - *start ||
	    demand_size > ULONG_MAX - demand)
		return -1;
	if (!*size || !demand_size)
		return 0;
	end = *start + *size;
	demand_end = demand + demand_size;
	left = max(*start, demand);
	right = min(end, demand_end);
	if (left < right) {
		extra = min(8UL, end - right);
		right += extra;
		left -= min(8UL - extra, left - *start);
	} else if (*start >= demand_end) {
		left = *start;
		right = left + min(8UL, *size);
	} else {
		right = end;
		left = right - min(8UL, *size);
	}
	if (left == *start && right == end)
		return 0;
	*start = left;
	*size = right - left;
	*async = min(*async, right > demand_end ?
			right - max(left, demand_end) : 0UL);
	return 1;
}

/* Decimal only, one optional final newline, no embedded NUL or whitespace. */
static int ra_limit_parse(const char *buf, size_t count, int maximum, int *value)
{
	unsigned int parsed = 0, digit;
	size_t i;

	if (count && buf[count - 1] == '\n')
		count--;
	if (!count)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (buf[i] < '0' || buf[i] > '9')
			return -EINVAL;
		digit = buf[i] - '0';
		if (digit > maximum || parsed > (maximum - digit) / 10)
			return -EINVAL;
		parsed = parsed * 10 + digit;
	}
	*value = parsed;
	return 0;
}

struct ra_limit_config {
	struct cgroup_subsys_state *css;
	u64 generation;
	bool enabled;
};

static DEFINE_MUTEX(ra_limit_mutex);
static struct ra_limit_config __rcu *ra_limit_config;
static u64 ra_limit_generation;

static bool ra_limit_online(struct cgroup_subsys_state *css)
{
	return css && (READ_ONCE(css->flags) & CSS_ONLINE) && !css_is_dying(css);
}

/* Control writers alone sleep; retired CSS references outlive all readers. */
static void ra_limit_publish(struct ra_limit_config *next)
{
	struct ra_limit_config *old;

	old = rcu_dereference_protected(ra_limit_config,
				       lockdep_is_held(&ra_limit_mutex));
	next->generation = ++ra_limit_generation;
	rcu_assign_pointer(ra_limit_config, next);
	synchronize_rcu();
	if (old) {
		if (old->css)
			css_put(old->css);
		kfree(old);
	}
}

static ssize_t enable_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	struct ra_limit_config *cfg;
	bool enabled;

	rcu_read_lock();
	cfg = rcu_dereference(ra_limit_config);
	enabled = cfg && cfg->enabled;
	rcu_read_unlock();
	return scnprintf(buf, PAGE_SIZE, "%u\n", enabled);
}

static ssize_t enable_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	struct ra_limit_config *old, *next;
	int value, ret = 0;

	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID) ||
	    !capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (ra_limit_parse(buf, count, 1, &value))
		return -EINVAL;
	next = kzalloc(sizeof(*next), GFP_KERNEL);
	if (!next)
		return -ENOMEM;
	mutex_lock(&ra_limit_mutex);
	old = rcu_dereference_protected(ra_limit_config,
				       lockdep_is_held(&ra_limit_mutex));
	if (value && (!old || !ra_limit_online(old->css))) {
		ret = -ENODEV;
		goto out;
	}
	if (old && old->css) {
		css_get(old->css);
		next->css = old->css;
	}
	next->enabled = value;
	ra_limit_publish(next);
out:
	mutex_unlock(&ra_limit_mutex);
	if (ret)
		kfree(next);
	return ret ? ret : count;
}

static ssize_t bind_fd_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	struct ra_limit_config *next;
	struct cgroup_subsys_state *css;
	struct fd f;
	char path[32];
	int fd, ret;

	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID) ||
	    !capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (ra_limit_parse(buf, count, INT_MAX, &fd))
		return -EINVAL;
	f = fdget_raw(fd);
	if (!f.file)
		return -EBADF;
	css = css_tryget_online_from_dir(f.file->f_path.dentry,
				       &cpuset_cgrp_subsys);
	fdput(f);
	if (IS_ERR(css))
		return PTR_ERR(css);
	/* Only the v1 top-level background object, never an ancestor fallback. */
	ret = cgroup_path(css->cgroup, path, sizeof(path));
	if (ret < 0 || ret >= sizeof(path) || !css->parent || !css->cgroup->root->hierarchy_id ||
	    strcmp(path, "/background") || !ra_limit_online(css)) {
		css_put(css);
		return -EINVAL;
	}
	next = kzalloc(sizeof(*next), GFP_KERNEL);
	if (!next) {
		css_put(css);
		return -ENOMEM;
	}
	next->css = css;
	mutex_lock(&ra_limit_mutex);
	/* Rebinding requires a separate explicit enable operation. */
	next->enabled = false;
	ra_limit_publish(next);
	mutex_unlock(&ra_limit_mutex);
	return count;
}

static struct kobj_attribute enable_attr = __ATTR(enable, 0600,
						 enable_show, enable_store);
static struct kobj_attribute bind_fd_attr = __ATTR(bind_fd, 0200,
						  NULL, bind_fd_store);
static struct attribute *ra_limit_attrs[] = {
	&enable_attr.attr, &bind_fd_attr.attr, NULL,
};
static const struct attribute_group ra_limit_group = {
	.name = "readahead_limit",
	.attrs = ra_limit_attrs,
};

static int __init ra_limit_init(void)
{
	if (!mm_kobj)
		return -ENOMEM;
	return sysfs_create_group(mm_kobj, &ra_limit_group);
}
subsys_initcall(ra_limit_init);

bool readahead_limit(struct address_space *mapping, const struct ra_context *ctx,
		     pgoff_t *start, unsigned long *size, unsigned long *async,
		     bool force)
{
	struct ra_limit_config *cfg;
	struct cgroup_subsys_state *css;
	pgoff_t raw_start = *start;
	unsigned long raw_size = *size, raw_async = *async;
	u64 generation = 0, identity = 0, target_identity = 0;
	long available = -1;
	int reason = 0, clipped = 0;
	struct readahead_limit_sample sample;

	rcu_read_lock();
	cfg = rcu_dereference(ra_limit_config);
	if (cfg)
		generation = cfg->generation;
	if (!ctx) {
		reason = force ? 9 : 1; /* UNSUPPORTED_EXPLICIT / UNKNOWN_SOURCE */
		goto out;
	}
	if (!cfg || !cfg->enabled)
		goto out; /* DISABLED */
	css = task_css(current, cpuset_cgrp_id);
	if (!ra_limit_online(cfg->css) || !ra_limit_online(css)) {
		reason = 2; /* UNKNOWN_IDENTITY */
		goto out;
	}
	identity = cgroup_id(css->cgroup);
	target_identity = cgroup_id(cfg->css->cgroup);
	if (css != cfg->css) {
		reason = 3; /* NOT_BACKGROUND */
		goto out;
	}
	available = si_mem_available();
	if (available < 0) {
		reason = 4; /* UNKNOWN_PRESSURE */
		goto out;
	}
	if (available > (1536UL << (20 - PAGE_SHIFT))) {
		reason = 5; /* NOT_LOW */
		goto out;
	}
	clipped = ra_limit_range(start, size, async, ctx->demand,
				ctx->demand_size);
	reason = clipped < 0 ? 6 : clipped ? 8 : 7;
out:
	rcu_read_unlock();
	sample.force = force;
	sample.reason = reason;
	sample.identity = identity;
	sample.target_identity = target_identity;
	sample.available = available;
	sample.generation = generation;
	sample.source = ctx ? ctx->source : 0;
	sample.demand = ctx ? ctx->demand : 0;
	sample.demand_size = ctx ? ctx->demand_size : 0;
	sample.raw_start = raw_start;
	sample.raw_size = raw_size;
	sample.raw_async = raw_async;
	sample.final_start = *start;
	sample.final_size = *size;
	sample.final_async = *async;
	trace_filemap_readahead_decision(mapping, &sample);
	return clipped > 0;
}
