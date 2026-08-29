/*
 * Global task boosting
 *
 * Copyright (C) 2018 Samsung Electronics Co., Ltd
 * Park Bumgyu <bumgyu.park@samsung.com>
 *
 * Ported to Troika kernel (adapted from Mint kernel)
 * Note: gb_qos_update_request and global_boost sysfs are in global_boost.c
 */

#include <linux/sched.h>
#include <linux/kobject.h>
#include <linux/ems.h>

#include "ems.h"
#include "../sched.h"
#include "../tune.h"

#include <trace/events/ems.h>

static int task_boost;

static ssize_t show_task_boost(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", task_boost);
}

static ssize_t store_task_boost(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf,
		size_t count)
{
	int boost;

	if (sscanf(buf, "%d", &boost) != 1)
		return -EINVAL;

	/* ignore if requested mode is out of range */
	if (boost < 0 || boost >= 32768)
		return -EINVAL;

	task_boost = boost;

	return count;
}

/******************************************************************************
 * global task boost                                                          *
 ******************************************************************************/
static int global_task_boost[CGROUP_COUNT] = {0, };

static ssize_t show_global_task_boost(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d %d %d %d %d\n",
			global_task_boost[CGROUP_ROOT],
			global_task_boost[CGROUP_FOREGROUND],
			global_task_boost[CGROUP_BACKGROUND],
			global_task_boost[CGROUP_TOPAPP],
			global_task_boost[CGROUP_RT]);
}

static ssize_t store_global_task_boost(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf,
		size_t count)
{
	unsigned int input;

	if (!sscanf(buf, "%d", &input))
		return -EINVAL;

	if (input > CGROUP_COUNT)
		return -EINVAL;

	/* Set all groups at once */
	{
		int i;
		for (i = 0; i < CGROUP_COUNT; i++)
			global_task_boost[i] = !!input;
	}

	return count;
}

static struct kobj_attribute task_boost_attr =
__ATTR(task_boost, 0644, show_task_boost, store_task_boost);
static struct kobj_attribute global_task_boost_attr =
__ATTR(global_task_boost, 0644, show_global_task_boost, store_global_task_boost);

static int __init init_boost_sysfs(void)
{
	int ret;

	ret = sysfs_create_file(ems_kobj, &task_boost_attr.attr);
	if (ret)
		pr_err("%s: failed to create task boost sysfs file\n", __func__);

	ret = sysfs_create_file(ems_kobj, &global_task_boost_attr.attr);
	if (ret)
		pr_err("%s: failed to create global task boost sysfs file\n", __func__);

	return 0;
}
late_initcall(init_boost_sysfs);

/*
 * Returns the biggest value in the global boost list. In the current policy,
 * a value greater than 0 is unconditionally boosting. The size of the value
 * is meaningless.
 */
int ems_boot_boost(void)
{
	u64 now = ktime_to_us(ktime_get());

	/* init boost duration = 60s */
	if (now < 60 * USEC_PER_SEC)
		return EMS_INIT_BOOST;

	/* booting boost duration = 120s */
	if (now < 120 * USEC_PER_SEC)
		return EMS_BOOT_BOOST;

	return 0;
}

int ems_global_boost(void)
{
	return global_boosted();
}

int ems_global_task_boost(int cgroup_idx)
{
	if (!ems_global_boost())
		return 0;

	/* enable for all groups if enabled on root */
	if (global_task_boost[CGROUP_ROOT])
		return 1;

	if (cgroup_idx >= CGROUP_COUNT)
		return 0;

	return global_task_boost[cgroup_idx];
}

int ems_task_boost(void)
{
	return task_boost;
}
