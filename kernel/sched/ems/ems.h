/*
 * Copyright (c) 2018 Samsung Electronics Co., Ltd
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

#include "../sched-pelt.h"

/* Forward declaration for Troika's cpu_util (in sched.h) */
extern unsigned long cpu_util(int cpu);

#define cpu_selected(cpu)	(cpu >= 0)
#define tsk_cpus_allowed(tsk)	(&(tsk)->cpus_allowed)

extern struct kobject *ems_kobj;

extern int select_service_cpu(struct task_struct *p);
extern int ontime_task_wakeup(struct task_struct *p, int sync);
extern int select_perf_cpu(struct task_struct *p);
extern int global_boosting(struct task_struct *p);
extern int global_boosted(void);
extern int select_energy_cpu(struct task_struct *p, int prev_cpu, int sd_flag, int sync);
extern unsigned int calculate_energy(struct task_struct *p, int target_cpu);
extern int band_play_cpu(struct task_struct *p);

#ifdef CONFIG_SCHED_TUNE
extern int prefer_perf_cpu(struct task_struct *p);
extern int prefer_idle_cpu(struct task_struct *p);
extern int group_balancing(struct task_struct *p);
#else
static inline int prefer_perf_cpu(struct task_struct *p) { return -1; }
static inline int prefer_idle_cpu(struct task_struct *p) { return -1; }
#endif

extern unsigned long task_util(struct task_struct *p);
extern int cpu_util_wake(int cpu, struct task_struct *p);
extern unsigned long task_util_est(struct task_struct *p);
extern unsigned int get_cpu_mips(unsigned int cpu);
extern unsigned int get_cpu_max_capacity(unsigned int cpu);

extern unsigned long boosted_task_util(struct task_struct *p);

/* maximum count of tracking tasks in runqueue */
#define TRACK_TASK_COUNT	5

/* CPU capacity boundaries */
#define MIN_CAPACITY_CPU	0
#define MAX_CAPACITY_CPU	(NR_CPUS - 1)

/* Multi-load task util wrappers (Troika uses standard PELT) */
static inline unsigned long ml_task_util(struct task_struct *p)
{
	return task_util(p);
}

static inline unsigned long ml_cpu_util(int cpu)
{
	return cpu_util(cpu);
}

/* Profile and scheduling status */
struct system_profile_data {
	int			busy_cpu_count;
	int			heavy_task_count;
	int			misfit_task_count;

	unsigned long		cpu_util_sum;
	unsigned long		heavy_task_util_sum;
	unsigned long		misfit_task_util_sum;
	unsigned long		heaviest_task_util;

	unsigned long		cpu_util[NR_CPUS];
};

#define BUSY_CPU_RATIO		(150)
#define HEAVY_TASK_UTIL_RATIO	(40)
#define MISFIT_TASK_UTIL_RATIO	(80)
#define check_busy(util, cap)	((util * 100) >= (cap * 80))

extern const unsigned int et_get_max_capacity(void);

static inline int is_heavy_task_util(unsigned long util)
{
	return (util * 100) >= (et_get_max_capacity() * HEAVY_TASK_UTIL_RATIO);
}

static inline int is_misfit_task_util(unsigned long util)
{
	return (util * 100) >= (et_get_max_capacity() * MISFIT_TASK_UTIL_RATIO);
}

static inline int profile_sched_init(void) { return 0; }
static inline int profile_sched_data(void) { return 0; }
static inline void get_system_sched_data(struct system_profile_data *data) { }

static inline void monitor_sysbusy(void) { }
static inline int sysbusy_schedule(struct task_struct *p, int prev_cpu) { return prev_cpu; }
static inline int sysbusy_init(void) { return 0; }
static inline int sysbusy_sysfs_init(void) { return 0; }
static inline void somac_tasks(void) { }

extern int schedtune_task_group_idx(struct task_struct *p);

static inline struct task_struct *task_of(struct sched_entity *se)
{
	return container_of(se, struct task_struct, se);
}
