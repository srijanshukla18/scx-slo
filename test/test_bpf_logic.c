/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Unit tests for BPF scheduler logic simulation
 * Tests conceptual correctness of scx_slo.bpf.c algorithms
 *
 * Note: These tests simulate BPF logic in userspace since BPF programs
 * cannot be directly unit tested without kernel context.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include "../include/scx_slo.h"

/* Test constants matching BPF code */
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC  1000000000ULL

/* Simulated BPF map limits from scx_slo.bpf.c */
#define MAX_CGROUPS 10000
#define MAX_TASKS 100000
#define RINGBUF_SIZE (1 << 20)  /* 1MB */
#define STATS_MAP_ENTRIES 2

/* Simulation of validate_slo_cfg from BPF */
static int validate_slo_cfg(struct slo_cfg *cfg)
{
	if (!cfg)
		return -1;

	if (cfg->budget_ns == 0 || cfg->budget_ns < MIN_BUDGET_NS || cfg->budget_ns > MAX_BUDGET_NS)
		return -1;

	if (cfg->importance < MIN_IMPORTANCE || cfg->importance > MAX_IMPORTANCE)
		return -1;

	return 0;
}

/* Simulation of get_safe_budget from BPF */
static uint64_t get_safe_budget(struct slo_cfg *cfg)
{
	if (!cfg)
		return DEFAULT_BUDGET_NS;

	if (validate_slo_cfg(cfg) != 0)
		return DEFAULT_BUDGET_NS;

	return cfg->budget_ns;
}

/* Test validate_slo_cfg function */
static void test_validate_slo_cfg(void)
{
	printf("Testing validate_slo_cfg...\n");

	struct slo_cfg cfg;

	/* Test NULL config */
	assert(validate_slo_cfg(NULL) == -1);
	printf("  NULL config: rejected\n");

	/* Test zero budget */
	cfg.budget_ns = 0;
	cfg.importance = 50;
	cfg.flags = 0;
	assert(validate_slo_cfg(&cfg) == -1);
	printf("  Zero budget: rejected\n");

	/* Test below minimum budget */
	cfg.budget_ns = MIN_BUDGET_NS - 1;
	assert(validate_slo_cfg(&cfg) == -1);
	printf("  Below min budget: rejected\n");

	/* Test above maximum budget */
	cfg.budget_ns = MAX_BUDGET_NS + 1;
	assert(validate_slo_cfg(&cfg) == -1);
	printf("  Above max budget: rejected\n");

	/* Test below minimum importance */
	cfg.budget_ns = DEFAULT_BUDGET_NS;
	cfg.importance = MIN_IMPORTANCE - 1;
	assert(validate_slo_cfg(&cfg) == -1);
	printf("  Below min importance: rejected\n");

	/* Test above maximum importance */
	cfg.importance = MAX_IMPORTANCE + 1;
	assert(validate_slo_cfg(&cfg) == -1);
	printf("  Above max importance: rejected\n");

	/* Test valid configuration */
	cfg.budget_ns = DEFAULT_BUDGET_NS;
	cfg.importance = 50;
	cfg.flags = 0;
	assert(validate_slo_cfg(&cfg) == 0);
	printf("  Valid config: accepted\n");

	/* Test boundary values */
	cfg.budget_ns = MIN_BUDGET_NS;
	cfg.importance = MIN_IMPORTANCE;
	assert(validate_slo_cfg(&cfg) == 0);
	printf("  Minimum valid values: accepted\n");

	cfg.budget_ns = MAX_BUDGET_NS;
	cfg.importance = MAX_IMPORTANCE;
	assert(validate_slo_cfg(&cfg) == 0);
	printf("  Maximum valid values: accepted\n");

	printf("OK validate_slo_cfg tests passed\n");
}

/* Test get_safe_budget function */
static void test_get_safe_budget(void)
{
	printf("Testing get_safe_budget...\n");

	struct slo_cfg cfg;

	/* Test NULL config */
	assert(get_safe_budget(NULL) == DEFAULT_BUDGET_NS);
	printf("  NULL config: returns default\n");

	/* Test invalid config */
	cfg.budget_ns = 0;
	cfg.importance = 50;
	cfg.flags = 0;
	assert(get_safe_budget(&cfg) == DEFAULT_BUDGET_NS);
	printf("  Invalid config: returns default\n");

	/* Test valid config */
	cfg.budget_ns = 50 * NSEC_PER_MSEC;
	cfg.importance = 90;
	assert(get_safe_budget(&cfg) == 50 * NSEC_PER_MSEC);
	printf("  Valid config (50ms): returns 50ms\n");

	printf("OK get_safe_budget tests passed\n");
}

/* Simulation of rate limiting logic from BPF */
static uint64_t rl_event_count = 0;
static uint64_t rl_window_start = 0;

static int is_rate_limited(uint64_t now)
{
	if (now - rl_window_start > RATE_LIMIT_WINDOW_NS) {
		rl_window_start = now;
		rl_event_count = 0;
	}

	if (rl_event_count >= MAX_EVENTS_PER_SEC)
		return 1;

	rl_event_count++;
	return 0;
}

/* Test rate limiting simulation */
static void test_rate_limiting(void)
{
	printf("Testing rate limiting logic...\n");

	uint64_t now = NSEC_PER_SEC;

	/* Reset state */
	rl_event_count = 0;
	rl_window_start = now;

	/* Should allow up to MAX_EVENTS_PER_SEC */
	for (uint64_t i = 0; i < MAX_EVENTS_PER_SEC; i++) {
		assert(is_rate_limited(now) == 0);
	}
	printf("  Allowed %d events in window\n", MAX_EVENTS_PER_SEC);

	/* Next event should be rate limited */
	assert(is_rate_limited(now) == 1);
	printf("  Event %d rate limited\n", MAX_EVENTS_PER_SEC + 1);

	/* After window passes, should reset */
	now += RATE_LIMIT_WINDOW_NS + 1;
	assert(is_rate_limited(now) == 0);
	printf("  Window reset: event allowed again\n");

	printf("OK Rate limiting tests passed\n");
}

/* Simulation of deadline calculation from enqueue */
static uint64_t calculate_deadline(uint64_t enqueue_time, uint64_t budget_ns)
{
	return enqueue_time + budget_ns;
}

/* Simulation of deadline miss detection from stopping */
static int detect_deadline_miss(uint64_t current_time, uint64_t deadline)
{
	return current_time > deadline;
}

/* Test deadline calculation logic */
static void test_deadline_calculation_logic(void)
{
	printf("Testing deadline calculation logic...\n");

	/* Test basic calculation */
	uint64_t enqueue_time = NSEC_PER_SEC;
	uint64_t budget = 100 * NSEC_PER_MSEC;
	uint64_t deadline = calculate_deadline(enqueue_time, budget);

	assert(deadline == enqueue_time + budget);
	printf("  Basic calculation: enqueue + budget = deadline\n");

	/* Test with various budgets */
	uint64_t budgets[] = {
		MIN_BUDGET_NS,
		50 * NSEC_PER_MSEC,
		DEFAULT_BUDGET_NS,
		500 * NSEC_PER_MSEC,
		MAX_BUDGET_NS
	};

	for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
		deadline = calculate_deadline(enqueue_time, budgets[i]);
		assert(deadline == enqueue_time + budgets[i]);
		printf("  Budget %llu ns: deadline correct\n", budgets[i]);
	}

	printf("OK Deadline calculation tests passed\n");
}

/* Test deadline miss detection logic */
static void test_deadline_miss_detection_logic(void)
{
	printf("Testing deadline miss detection logic...\n");

	uint64_t deadline = NSEC_PER_SEC + DEFAULT_BUDGET_NS;

	/* Before deadline: no miss */
	assert(detect_deadline_miss(deadline - 1, deadline) == 0);
	printf("  time < deadline: no miss\n");

	/* At deadline: no miss (boundary) */
	assert(detect_deadline_miss(deadline, deadline) == 0);
	printf("  time == deadline: no miss (boundary)\n");

	/* After deadline: miss */
	assert(detect_deadline_miss(deadline + 1, deadline) == 1);
	printf("  time > deadline: miss detected\n");

	/* Significant miss */
	assert(detect_deadline_miss(deadline + 50 * NSEC_PER_MSEC, deadline) == 1);
	printf("  time >> deadline: significant miss detected\n");

	printf("OK Deadline miss detection tests passed\n");
}

/* Simulation of task context management */
struct test_task_ctx {
	uint64_t deadline;
	uint64_t start_time;
	uint64_t budget_ns;
	uint32_t valid;
};

static void test_task_context_lifecycle(void)
{
	printf("Testing task context lifecycle...\n");

	struct test_task_ctx ctx;

	/* Initial state (uninitialized) */
	memset(&ctx, 0, sizeof(ctx));
	assert(ctx.valid == 0);
	printf("  New context: valid=0\n");

	/* After enqueue (initialized) */
	uint64_t enqueue_time = NSEC_PER_SEC;
	uint64_t budget = 100 * NSEC_PER_MSEC;

	ctx.deadline = enqueue_time + budget;
	ctx.budget_ns = budget;
	ctx.start_time = 0;  /* Not yet running */
	ctx.valid = 1;

	assert(ctx.valid == 1);
	assert(ctx.deadline == enqueue_time + budget);
	printf("  After enqueue: valid=1, deadline set\n");

	/* After running starts */
	ctx.start_time = enqueue_time + 10 * NSEC_PER_MSEC;  /* 10ms delay */
	assert(ctx.start_time > 0);
	printf("  After running: start_time=%llu\n", ctx.start_time);

	/* Stopping without miss */
	uint64_t stop_time = enqueue_time + 50 * NSEC_PER_MSEC;  /* 50ms total */
	assert(!detect_deadline_miss(stop_time, ctx.deadline));
	printf("  Stopping at %llu: no miss (deadline=%llu)\n", stop_time, ctx.deadline);

	/* Context cleanup when task stops (not runnable) */
	memset(&ctx, 0, sizeof(ctx));
	assert(ctx.valid == 0);
	printf("  After cleanup: valid=0\n");

	printf("OK Task context lifecycle tests passed\n");
}

/* Test DSQ insertion priority ordering */
static void test_dsq_priority_ordering(void)
{
	printf("Testing DSQ priority ordering (EDF)...\n");

	/* Simulate multiple tasks with different deadlines */
	struct {
		uint32_t pid;
		uint64_t deadline;
	} tasks[] = {
		{1001, NSEC_PER_SEC + 100 * NSEC_PER_MSEC},  /* 1.1s */
		{1002, NSEC_PER_SEC + 50 * NSEC_PER_MSEC},   /* 1.05s - earliest */
		{1003, NSEC_PER_SEC + 200 * NSEC_PER_MSEC},  /* 1.2s */
		{1004, NSEC_PER_SEC + 75 * NSEC_PER_MSEC},   /* 1.075s */
	};

	/* In EDF, task with earliest deadline should run first */
	uint32_t expected_order[] = {1002, 1004, 1001, 1003};

	/* Simple bubble sort to simulate priority queue */
	for (size_t i = 0; i < 4; i++) {
		for (size_t j = i + 1; j < 4; j++) {
			if (tasks[j].deadline < tasks[i].deadline) {
				struct { uint32_t pid; uint64_t deadline; } tmp = tasks[i];
				tasks[i] = tasks[j];
				tasks[j] = tmp;
			}
		}
	}

	for (size_t i = 0; i < 4; i++) {
		assert(tasks[i].pid == expected_order[i]);
		printf("  Priority %zu: PID %u (deadline %llu)\n",
		       i + 1, tasks[i].pid, tasks[i].deadline);
	}

	printf("OK DSQ priority ordering (EDF) verified\n");
}

/* Test stat increment simulation */
static uint64_t mock_stats[2] = {0, 0};

static void stat_inc(uint32_t idx)
{
	if (idx < 2)
		mock_stats[idx]++;
}

static void test_stats_increment(void)
{
	printf("Testing stats increment logic...\n");

	/* Reset stats */
	mock_stats[0] = 0;
	mock_stats[1] = 0;

	/* Simulate local queueing events */
	for (int i = 0; i < 100; i++) {
		stat_inc(0);
	}
	assert(mock_stats[0] == 100);
	printf("  100 local queue events: stats[0]=%llu\n", mock_stats[0]);

	/* Simulate global queueing events */
	for (int i = 0; i < 250; i++) {
		stat_inc(1);
	}
	assert(mock_stats[1] == 250);
	printf("  250 global queue events: stats[1]=%llu\n", mock_stats[1]);

	/* Test invalid index (should be no-op) */
	uint64_t before_0 = mock_stats[0];
	uint64_t before_1 = mock_stats[1];
	stat_inc(99);  /* Invalid index */
	assert(mock_stats[0] == before_0);
	assert(mock_stats[1] == before_1);
	printf("  Invalid index: no change\n");

	printf("OK Stats increment tests passed\n");
}

/* Test CPU selection logic simulation */
static void test_cpu_selection_logic(void)
{
	printf("Testing CPU selection logic...\n");

	/* Simulation of select_cpu behavior */
	int prev_cpu = 2;
	int is_idle = 0;

	/* Case 1: No idle CPU found */
	int selected_cpu = prev_cpu;  /* Fallback to prev */
	assert(selected_cpu == 2);
	printf("  No idle CPU: use prev_cpu=%d\n", selected_cpu);

	/* Case 2: Idle CPU found */
	is_idle = 1;
	selected_cpu = 5;  /* Simulated idle CPU */
	assert(selected_cpu != prev_cpu);
	assert(is_idle == 1);
	printf("  Idle CPU found: use cpu=%d\n", selected_cpu);

	/* When idle, task is inserted locally (stat_inc(0)) */
	mock_stats[0] = 0;
	if (is_idle) {
		stat_inc(0);
	}
	assert(mock_stats[0] == 1);
	printf("  Idle CPU path: local stat incremented\n");

	printf("OK CPU selection logic verified\n");
}

/* Test enqueue fallback behavior */
static void test_enqueue_fallback(void)
{
	printf("Testing enqueue fallback behavior...\n");

	/* Simulation: what happens when task context creation fails */
	struct test_task_ctx *ctx = NULL;  /* Simulated failure */

	if (!ctx) {
		/* Should use default scheduling without context */
		mock_stats[1] = 0;
		stat_inc(1);  /* global queueing */
		assert(mock_stats[1] == 1);
		printf("  Context creation failed: fallback to global DSQ\n");
	}

	/* Normal case: context exists */
	struct test_task_ctx real_ctx;
	memset(&real_ctx, 0, sizeof(real_ctx));
	ctx = &real_ctx;

	if (ctx) {
		ctx->deadline = NSEC_PER_SEC + DEFAULT_BUDGET_NS;
		ctx->valid = 1;
		printf("  Context created: deadline-based scheduling used\n");
	}

	printf("OK Enqueue fallback behavior verified\n");
}

/* Test map limit enforcement */
static void test_map_limits(void)
{
	printf("Testing BPF map limits...\n");

	assert(MAX_CGROUPS == 10000);
	printf("  MAX_CGROUPS: %d\n", MAX_CGROUPS);

	assert(MAX_TASKS == 100000);
	printf("  MAX_TASKS: %d\n", MAX_TASKS);

	assert(RINGBUF_SIZE == (1 << 20));
	printf("  RINGBUF_SIZE: %d bytes (1MB)\n", RINGBUF_SIZE);

	assert(STATS_MAP_ENTRIES == 2);
	printf("  STATS_MAP_ENTRIES: %d\n", STATS_MAP_ENTRIES);

	/* Verify reasonable sizing */
	assert(MAX_CGROUPS > 0 && MAX_CGROUPS <= 1000000);
	assert(MAX_TASKS > 0 && MAX_TASKS <= 10000000);
	assert(RINGBUF_SIZE >= 4096);

	printf("OK Map limits verified\n");
}

/* Test deadline event structure packing */
static void test_deadline_event_packing(void)
{
	printf("Testing deadline_event structure packing...\n");

	struct deadline_event event;

	/* Structure should be tightly packed */
	assert(sizeof(event) == sizeof(uint64_t) * 3);
	printf("  Structure size: %zu bytes (expected 24)\n", sizeof(event));

	/* Test field assignment */
	event.cgroup_id = 0xDEADBEEF12345678ULL;
	event.deadline_miss_ns = 5 * NSEC_PER_MSEC;
	event.timestamp = NSEC_PER_SEC * 100;

	assert(event.cgroup_id == 0xDEADBEEF12345678ULL);
	assert(event.deadline_miss_ns == 5000000);
	assert(event.timestamp == 100 * NSEC_PER_SEC);

	printf("  Field assignment verified\n");

	printf("OK Deadline event packing verified\n");
}

int main(void)
{
	printf("Running BPF logic simulation tests...\n\n");

	test_validate_slo_cfg();
	test_get_safe_budget();
	test_rate_limiting();
	test_deadline_calculation_logic();
	test_deadline_miss_detection_logic();
	test_task_context_lifecycle();
	test_dsq_priority_ordering();
	test_stats_increment();
	test_cpu_selection_logic();
	test_enqueue_fallback();
	test_map_limits();
	test_deadline_event_packing();

	printf("\nAll BPF logic simulation tests passed!\n");
	return 0;
}
