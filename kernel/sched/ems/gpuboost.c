/*
 * GPU global task boosting
 *
 * Copyright (C) 2023 tenseventyseven
 * TenSeventy7 <git@tenseventyseven.cf>
 * Copyright (C) 2018 Samsung Electronics Co., Ltd
 * Park Bumgyu <bumgyu.park@samsung.com>
 *
 * Ported to Troika kernel (adapted from Mint kernel)
 */

#include <linux/sched.h>
#include <linux/ems.h>

#include "ems.h"
#include "../sched.h"
#include "../tune.h"

#define QOS_EMS_MIN_FREQ 764000

/******************************************************************************
 * gpexbe task boost                                                          *
 ******************************************************************************/
static s32 gpu_boost_freq = QOS_EMS_MIN_FREQ;
static struct gb_qos_request gpu_boost_req = {
	.name = "gpexbe_boost",
};

void ems_gpu_boost_update(s32 gpu_cur_freq) {
	u32 req = 0;

	if (gpu_boost_freq && gpu_cur_freq >= gpu_boost_freq)
		req = 100;

	gb_qos_update_request(&gpu_boost_req, req);
}
