/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SLO-aware scheduler (scx-slo) - shared definitions
 *
 * This header is designed to work in both kernel BPF and userspace contexts.
 */
#ifndef __SCX_SLO_H
#define __SCX_SLO_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint64_t __u64;
typedef uint32_t __u32;
#endif

/* SLO configuration per cgroup */
struct slo_cfg {
	__u64 budget_ns;      /* Latency budget in nanoseconds */
	__u32 importance;     /* Relative importance (1-100) */
	__u32 flags;          /* Configuration flags */
};

/* Per-task scheduling context */
struct slo_task_ctx {
	__u64 deadline;       /* When this task must complete by */
	__u64 start_time;     /* When task started running (for miss detection) */
	__u64 budget_ns;      /* Task's allocated budget */
	__u32 valid;          /* Whether this context is initialized */
};

/* Deadline event structure for ring buffer */
struct deadline_event {
	__u64 cgroup_id;
	__u64 deadline_miss_ns;
	__u64 timestamp;
};

/* SLO budget constants with validation bounds */
#define DEFAULT_BUDGET_NS (100 * 1000000ULL)  /* 100ms default */
#define MIN_BUDGET_NS     (1 * 1000000ULL)    /* 1ms minimum */
#define MAX_BUDGET_NS     (10 * 1000000000ULL) /* 10s maximum */

/* Importance value bounds */
#define MIN_IMPORTANCE 1
#define MAX_IMPORTANCE 100

/* Rate limiting for ring buffer events */
#define MAX_EVENTS_PER_SEC 1000
#define RATE_LIMIT_WINDOW_NS (1 * 1000000000ULL)

#endif /* __SCX_SLO_H */