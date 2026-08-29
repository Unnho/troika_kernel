/*
 * Copyright (c) 2017 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/plist.h>
#include <linux/sched/idle.h>
#include <linux/sched/topology.h>

enum task_cgroup {
	CGROUP_ROOT,
	CGROUP_FOREGROUND,
	CGROUP_BACKGROUND,
	CGROUP_TOPAPP,
	CGROUP_RT,
	CGROUP_COUNT,
};

struct gb_qos_request {
	struct plist_node node;
	char *name;
	bool active;
};

/*
 * Helpers for converting millisecond timing to jiffy resolution
 */
#define MS_TO_JIFFIES(TIME) ((unsigned long)(TIME) / (MSEC_PER_SEC / HZ))

/*
 * Sysbusy
 */
enum sysbusy_state {
	SYSBUSY_STATE0 = 0,
	SYSBUSY_STATE1,
	SYSBUSY_STATE2,
	SYSBUSY_STATE3,
	NUM_OF_SYSBUSY_STATE,
};

#define SYSBUSY_CHECK_BOOST (0)
#define SYSBUSY_STATE_CHANGE	(1)

struct sysbusy_param {
	int monitor_interval;		/* tick (1 tick = 4ms) */
	int release_duration;		/* tick (1 tick = 4ms) */
	unsigned long allowed_next_state;
};

#define TICK_SEC		MS_TO_JIFFIES(1000)
#define BUSY_MONITOR_INTERVAL	MS_TO_JIFFIES(100)
static struct sysbusy_param sysbusy_params[] = {
	{
		/* SYSBUSY_STATE0 (sysbusy inactivation) */
		.monitor_interval	= MS_TO_JIFFIES(4),
		.release_duration	= 0,
		.allowed_next_state	= (1 << SYSBUSY_STATE1) |
					  (1 << SYSBUSY_STATE2) |
					  (1 << SYSBUSY_STATE3),
	},
	{
		/*
		 * TenSeventy7:
		 * Avoid instances where spiking loads, which is very frequent in
		 * Android, would cause 'sysbusy' levels 0-1 to switch and trigger quickly.
		 * Let the heavy load alleviate first by making STATE1 longer.
		 */
		/* SYSBUSY_STATE1 */
		.monitor_interval	= MS_TO_JIFFIES(16),
		.release_duration	= MS_TO_JIFFIES(500),
		.allowed_next_state	= (1 << SYSBUSY_STATE0) |
					  (1 << SYSBUSY_STATE2) |
					  (1 << SYSBUSY_STATE3),
	},
	{
		/* SYSBUSY_STATE2 */
		.monitor_interval	= BUSY_MONITOR_INTERVAL,
		.release_duration	= TICK_SEC * 3,
		.allowed_next_state	= (1 << SYSBUSY_STATE0) |
					  (1 << SYSBUSY_STATE3),
	},
	{
		/* SYSBUSY_STATE3 */
		.monitor_interval	= BUSY_MONITOR_INTERVAL,
		.release_duration	= TICK_SEC * 9,
		.allowed_next_state	= (1 << SYSBUSY_STATE0),
	},
};

#define SYSBUSY_SOMAC	SYSBUSY_STATE3

#define LEAVE_BAND	0

struct task_band {
	int id;
	pid_t tgid;
	raw_spinlock_t lock;

	struct list_head members;
	int member_count;
	struct cpumask playable_cpus;

	unsigned long util;
	unsigned long last_update_time;
};

#ifdef CONFIG_SCHED_EMS
extern struct kobject *ems_kobj;
extern unsigned int get_cpu_max_capacity(unsigned int cpu);

/* task util initialization */
extern void exynos_init_entity_util_avg(struct sched_entity *se);

/* active balance */
extern int exynos_need_active_balance(enum cpu_idle_type idle,
				struct sched_domain *sd, int src_cpu, int dst_cpu);

/* wakeup balance */
extern int
exynos_wakeup_balance(struct task_struct *p, int prev_cpu, int sd_flag, int sync);

/* ontime migration */
extern void ontime_migration(void);
extern int ontime_can_migration(struct task_struct *p, int cpu);
extern void ontime_update_load_avg(u64 delta, int cpu, unsigned long weight, struct sched_avg *sa);
extern void ontime_new_entity_load(struct task_struct *parent, struct sched_entity *se);
extern void ontime_trace_task_info(struct task_struct *p);

/* load balance trigger */
extern bool lbt_overutilized(int cpu, int level);
extern void update_lbt_overutil(int cpu, unsigned long capacity);

/* global boost */
extern void gb_qos_update_request(struct gb_qos_request *req, u32 new_value);

/* task band */
extern void sync_band(struct task_struct *p, bool join);
extern void newbie_join_band(struct task_struct *newbie);
extern int alloc_bands(void);
extern void update_band(struct task_struct *p, long old_util);
extern int band_playing(struct task_struct *p, int cpu);

/* ems boost */
#define EMS_INIT_BOOST 1
#define EMS_BOOT_BOOST 2

extern int ems_task_boost(void);
extern int ems_boot_boost(void);
extern int ems_global_boost(void);
extern int ems_global_task_boost(int cgroup_idx);
extern void ems_gpu_boost_update(s32 gpu_cur_freq);

extern const struct cpumask *cpu_slowest_mask(void);
extern const struct cpumask *cpu_fastest_mask(void);
extern inline bool et_cpu_slowest(int cpu);

extern int sysbusy_register_notifier(struct notifier_block *nb);
extern int sysbusy_unregister_notifier(struct notifier_block *nb);
extern int sysbusy_activated(void);
extern void monitor_sysbusy(void);
extern int sysbusy_schedule(struct task_struct *p, int prev_cpu);
extern void somac_tasks(void);
extern int sysbusy_on_somac(void);
extern int is_somac_ready(struct task_struct *p);
extern int sysbusy_init(void);
extern int sysbusy_sysfs_init(void);
extern int profile_sched_init(void);
extern int profile_sched_data(void);
#else
static inline void gb_qos_update_request(struct gb_qos_request *req, u32 new_value) { }
static inline bool et_cpu_slowest(int cpu)
{
	return false;
}

static inline int sysbusy_register_notifier(struct notifier_block *nb) { return 0; };
static inline int sysbusy_unregister_notifier(struct notifier_block *nb) { return 0; };
static inline int sysbusy_activated(void) { return 0; }
static inline void monitor_sysbusy(void) { }
static inline int sysbusy_schedule(struct task_struct *p, int prev_cpu) { return -1; }
static inline void somac_tasks(void) { }
static inline int sysbusy_on_somac(void) { return 0; }
static inline int is_somac_ready(struct task_struct *p) { return 0; }
static inline int sysbusy_init(void) { return 0; }
static inline int sysbusy_sysfs_init(void) { return 0; }
static inline int profile_sched_init(void) { return 0; }
static inline int profile_sched_data(void) { return 0; }
static inline int ems_task_boost(void) { return 0; }
static inline int ems_boot_boost(void) { return 0; }
static inline int ems_global_boost(void) { return 0; }
static inline int ems_global_task_boost(int cgroup_idx) { return 0; }
static inline void ems_gpu_boost_update(s32 gpu_cur_freq) { }
#endif /* CONFIG_SCHED_EMS */

#ifdef CONFIG_SIMPLIFIED_ENERGY_MODEL
extern void init_sched_energy_table(struct cpumask *cpus, int table_size,
				unsigned long *f_table, unsigned int *v_table,
				int max_f, int min_f);
#else
static inline void init_sched_energy_table(struct cpumask *cpus, int table_size,
				unsigned long *f_table, unsigned int *v_table,
				int max_f, int min_f) { }
#endif
