/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Integration tests for scx-slo scheduler
 * Tests end-to-end scenarios and component interactions
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "../include/scx_slo.h"

/* Test constants */
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC  1000000000ULL
#define MAX_CGROUP_PATH 512
#define MAX_TEST_TASKS 1000
#define MAX_TEST_CGROUPS 100

/* Simulated SLO map entry */
struct slo_map_entry {
	uint64_t cgroup_id;
	struct slo_cfg cfg;
	int in_use;
};

/* Simulated task context entry */
struct task_ctx_entry {
	uint32_t pid;
	struct slo_task_ctx ctx;
	int in_use;
};

/* Simulated deadline event */
struct deadline_event {
	uint64_t cgroup_id;
	uint64_t deadline_miss_ns;
	uint64_t timestamp;
};

/* Global simulation state */
static struct slo_map_entry slo_map[MAX_TEST_CGROUPS];
static struct task_ctx_entry task_map[MAX_TEST_TASKS];
static struct deadline_event event_log[10000];
static int event_log_count = 0;
static uint64_t stats_local = 0;
static uint64_t stats_global = 0;

/* Initialize simulation state */
static void init_simulation(void)
{
	memset(slo_map, 0, sizeof(slo_map));
	memset(task_map, 0, sizeof(task_map));
	memset(event_log, 0, sizeof(event_log));
	event_log_count = 0;
	stats_local = 0;
	stats_global = 0;
}

/* Simulate BPF map lookup for SLO config */
static struct slo_cfg *lookup_slo_cfg(uint64_t cgroup_id)
{
	for (int i = 0; i < MAX_TEST_CGROUPS; i++) {
		if (slo_map[i].in_use && slo_map[i].cgroup_id == cgroup_id) {
			return &slo_map[i].cfg;
		}
	}
	return NULL;
}

/* Simulate BPF map update for SLO config */
static int update_slo_cfg(uint64_t cgroup_id, struct slo_cfg *cfg)
{
	/* Validate config first */
	if (cfg->budget_ns < MIN_BUDGET_NS || cfg->budget_ns > MAX_BUDGET_NS)
		return -EINVAL;
	if (cfg->importance < MIN_IMPORTANCE || cfg->importance > MAX_IMPORTANCE)
		return -EINVAL;

	/* Find existing or empty slot */
	int empty_slot = -1;
	for (int i = 0; i < MAX_TEST_CGROUPS; i++) {
		if (slo_map[i].in_use && slo_map[i].cgroup_id == cgroup_id) {
			slo_map[i].cfg = *cfg;
			return 0;
		}
		if (!slo_map[i].in_use && empty_slot < 0) {
			empty_slot = i;
		}
	}

	if (empty_slot < 0)
		return -ENOMEM;

	slo_map[empty_slot].cgroup_id = cgroup_id;
	slo_map[empty_slot].cfg = *cfg;
	slo_map[empty_slot].in_use = 1;
	return 0;
}

/* Simulate task context lookup/create */
static struct slo_task_ctx *get_task_ctx(uint32_t pid)
{
	for (int i = 0; i < MAX_TEST_TASKS; i++) {
		if (task_map[i].in_use && task_map[i].pid == pid) {
			return &task_map[i].ctx;
		}
	}

	/* Create new entry */
	for (int i = 0; i < MAX_TEST_TASKS; i++) {
		if (!task_map[i].in_use) {
			task_map[i].pid = pid;
			memset(&task_map[i].ctx, 0, sizeof(task_map[i].ctx));
			task_map[i].in_use = 1;
			return &task_map[i].ctx;
		}
	}

	return NULL;
}

/* Delete task context */
static void delete_task_ctx(uint32_t pid)
{
	for (int i = 0; i < MAX_TEST_TASKS; i++) {
		if (task_map[i].in_use && task_map[i].pid == pid) {
			task_map[i].in_use = 0;
			return;
		}
	}
}

/* Log deadline event */
static void log_deadline_event(uint64_t cgroup_id, uint64_t miss_ns, uint64_t ts)
{
	if (event_log_count < 10000) {
		event_log[event_log_count].cgroup_id = cgroup_id;
		event_log[event_log_count].deadline_miss_ns = miss_ns;
		event_log[event_log_count].timestamp = ts;
		event_log_count++;
	}
}

/* Simulate enqueue operation */
static void sim_enqueue(uint32_t pid, uint64_t cgroup_id, uint64_t now)
{
	stats_global++;

	struct slo_cfg *cfg = lookup_slo_cfg(cgroup_id);
	uint64_t budget = cfg ? cfg->budget_ns : DEFAULT_BUDGET_NS;

	struct slo_task_ctx *ctx = get_task_ctx(pid);
	if (!ctx) {
		return;  /* Fallback case */
	}

	ctx->deadline = now + budget;
	ctx->budget_ns = budget;
	ctx->start_time = 0;
	ctx->valid = 1;
}

/* Simulate running operation */
static void sim_running(uint32_t pid, uint64_t now)
{
	struct slo_task_ctx *ctx = get_task_ctx(pid);
	if (ctx && ctx->valid) {
		ctx->start_time = now;
	}
}

/* Simulate stopping operation */
static void sim_stopping(uint32_t pid, uint64_t cgroup_id, uint64_t now, int runnable)
{
	for (int i = 0; i < MAX_TEST_TASKS; i++) {
		if (task_map[i].in_use && task_map[i].pid == pid) {
			struct slo_task_ctx *ctx = &task_map[i].ctx;

			if (ctx->valid && now > ctx->deadline) {
				log_deadline_event(cgroup_id, now - ctx->deadline, now);
			}

			if (!runnable) {
				task_map[i].in_use = 0;
			}
			return;
		}
	}
}

/* Test basic workflow: config -> enqueue -> run -> stop */
static void test_basic_workflow(void)
{
	printf("Testing basic workflow...\n");
	init_simulation();

	/* Step 1: Configure SLO for a cgroup */
	uint64_t cgroup_id = 12345;
	struct slo_cfg cfg = {
		.budget_ns = 50 * NSEC_PER_MSEC,
		.importance = 90,
		.flags = 0
	};

	int ret = update_slo_cfg(cgroup_id, &cfg);
	assert(ret == 0);
	printf("  Step 1: SLO config set (budget=50ms, importance=90)\n");

	/* Step 2: Task is enqueued */
	uint32_t pid = 1001;
	uint64_t enqueue_time = NSEC_PER_SEC;

	sim_enqueue(pid, cgroup_id, enqueue_time);
	assert(stats_global == 1);

	struct slo_task_ctx *ctx = get_task_ctx(pid);
	assert(ctx != NULL);
	assert(ctx->valid == 1);
	assert(ctx->deadline == enqueue_time + 50 * NSEC_PER_MSEC);
	printf("  Step 2: Task enqueued (deadline=%llu)\n", ctx->deadline);

	/* Step 3: Task starts running */
	uint64_t run_time = enqueue_time + 5 * NSEC_PER_MSEC;  /* 5ms queue wait */
	sim_running(pid, run_time);
	assert(ctx->start_time == run_time);
	printf("  Step 3: Task running (started at %llu)\n", ctx->start_time);

	/* Step 4: Task stops within deadline */
	uint64_t stop_time = run_time + 30 * NSEC_PER_MSEC;  /* 30ms execution */
	sim_stopping(pid, cgroup_id, stop_time, 0);

	/* Should not have logged a deadline miss */
	assert(event_log_count == 0);
	printf("  Step 4: Task stopped within deadline (no miss)\n");

	printf("OK Basic workflow test passed\n");
}

/* Test deadline miss scenario */
static void test_deadline_miss_scenario(void)
{
	printf("Testing deadline miss scenario...\n");
	init_simulation();

	/* Configure short budget */
	uint64_t cgroup_id = 99999;
	struct slo_cfg cfg = {
		.budget_ns = 20 * NSEC_PER_MSEC,  /* 20ms budget */
		.importance = 95,
		.flags = 0
	};
	update_slo_cfg(cgroup_id, &cfg);

	/* Enqueue task */
	uint32_t pid = 2001;
	uint64_t enqueue_time = NSEC_PER_SEC;
	sim_enqueue(pid, cgroup_id, enqueue_time);

	/* Task starts late due to contention */
	uint64_t run_time = enqueue_time + 15 * NSEC_PER_MSEC;  /* 15ms delayed */
	sim_running(pid, run_time);

	/* Task runs for 10ms but total time exceeds deadline */
	uint64_t stop_time = run_time + 10 * NSEC_PER_MSEC;  /* 25ms total */
	sim_stopping(pid, cgroup_id, stop_time, 0);

	/* Should have recorded deadline miss */
	assert(event_log_count == 1);
	assert(event_log[0].cgroup_id == cgroup_id);
	assert(event_log[0].deadline_miss_ns == 5 * NSEC_PER_MSEC);  /* 5ms late */
	printf("  Deadline miss detected: %llu ns late\n",
	       event_log[0].deadline_miss_ns);

	printf("OK Deadline miss scenario test passed\n");
}

/* Test multiple tasks with different SLOs */
static void test_multi_task_multi_slo(void)
{
	printf("Testing multiple tasks with different SLOs...\n");
	init_simulation();

	/* Configure multiple SLOs */
	struct {
		uint64_t cgroup_id;
		uint64_t budget_ms;
		uint32_t importance;
	} slos[] = {
		{1000, 10, 99},   /* Critical - 10ms */
		{2000, 50, 80},   /* Standard - 50ms */
		{3000, 200, 50},  /* Batch - 200ms */
	};

	for (size_t i = 0; i < 3; i++) {
		struct slo_cfg cfg = {
			.budget_ns = slos[i].budget_ms * NSEC_PER_MSEC,
			.importance = slos[i].importance,
			.flags = 0
		};
		update_slo_cfg(slos[i].cgroup_id, &cfg);
	}
	printf("  Configured 3 SLO tiers\n");

	/* Enqueue tasks from each cgroup */
	uint64_t now = NSEC_PER_SEC;

	for (int tier = 0; tier < 3; tier++) {
		for (int t = 0; t < 10; t++) {
			uint32_t pid = (tier + 1) * 1000 + t;
			sim_enqueue(pid, slos[tier].cgroup_id, now);
		}
	}
	assert(stats_global == 30);
	printf("  Enqueued 30 tasks across 3 tiers\n");

	/* Verify deadlines are set correctly */
	for (int tier = 0; tier < 3; tier++) {
		uint32_t pid = (tier + 1) * 1000;
		struct slo_task_ctx *ctx = get_task_ctx(pid);
		assert(ctx != NULL);
		uint64_t expected_deadline = now + slos[tier].budget_ms * NSEC_PER_MSEC;
		assert(ctx->deadline == expected_deadline);
		printf("  Tier %d deadline verified: %llu\n", tier + 1, ctx->deadline);
	}

	printf("OK Multi-task multi-SLO test passed\n");
}

/* Test default budget fallback */
static void test_default_budget_fallback(void)
{
	printf("Testing default budget fallback...\n");
	init_simulation();

	/* Enqueue task without configured SLO */
	uint64_t unknown_cgroup = 777777;
	uint32_t pid = 3001;
	uint64_t now = NSEC_PER_SEC;

	sim_enqueue(pid, unknown_cgroup, now);

	struct slo_task_ctx *ctx = get_task_ctx(pid);
	assert(ctx != NULL);
	assert(ctx->budget_ns == DEFAULT_BUDGET_NS);
	assert(ctx->deadline == now + DEFAULT_BUDGET_NS);

	printf("  Task with unknown cgroup got default budget: %llu ns\n",
	       ctx->budget_ns);

	printf("OK Default budget fallback test passed\n");
}

/* Test task lifecycle - multiple enqueue/stop cycles */
static void test_task_lifecycle_cycles(void)
{
	printf("Testing task lifecycle cycles...\n");
	init_simulation();

	uint64_t cgroup_id = 5000;
	struct slo_cfg cfg = {
		.budget_ns = 100 * NSEC_PER_MSEC,
		.importance = 70,
		.flags = 0
	};
	update_slo_cfg(cgroup_id, &cfg);

	uint32_t pid = 4001;
	uint64_t now = NSEC_PER_SEC;

	/* Cycle 1: Enqueue -> Run -> Stop (non-runnable) */
	sim_enqueue(pid, cgroup_id, now);
	sim_running(pid, now + 5 * NSEC_PER_MSEC);
	sim_stopping(pid, cgroup_id, now + 20 * NSEC_PER_MSEC, 0);

	/* Task context should be cleaned up */
	struct slo_task_ctx *ctx = get_task_ctx(pid);
	/* get_task_ctx creates new one, check it's fresh */
	assert(ctx->valid == 0);
	printf("  Cycle 1: Task stopped non-runnable, context cleaned\n");

	/* Cycle 2: New enqueue */
	now += NSEC_PER_SEC;
	sim_enqueue(pid, cgroup_id, now);
	ctx = get_task_ctx(pid);
	assert(ctx->valid == 1);
	printf("  Cycle 2: Task re-enqueued with fresh context\n");

	/* Cycle 3: Stop but still runnable (preempted) */
	sim_running(pid, now + 1 * NSEC_PER_MSEC);
	sim_stopping(pid, cgroup_id, now + 10 * NSEC_PER_MSEC, 1);

	/* Context should be preserved when runnable */
	ctx = get_task_ctx(pid);
	assert(ctx->valid == 1);
	printf("  Cycle 3: Task preempted, context preserved\n");

	printf("OK Task lifecycle cycles test passed\n");
}

/* Test stress scenario - many concurrent tasks */
static void test_stress_many_tasks(void)
{
	printf("Testing stress scenario with many tasks...\n");
	init_simulation();

	/* Create diverse SLO configurations */
	for (int i = 0; i < 50; i++) {
		struct slo_cfg cfg = {
			.budget_ns = ((i % 10) + 1) * 10 * NSEC_PER_MSEC,  /* 10-100ms */
			.importance = (i % 100) + 1,
			.flags = 0
		};
		update_slo_cfg(i + 1000, &cfg);
	}

	/* Enqueue many tasks */
	uint64_t now = NSEC_PER_SEC;
	int tasks_created = 0;

	for (int i = 0; i < MAX_TEST_TASKS; i++) {
		uint64_t cgroup = (i % 50) + 1000;
		sim_enqueue(i + 1, cgroup, now + (i * 1000));  /* Stagger by 1us */

		struct slo_task_ctx *ctx = get_task_ctx(i + 1);
		if (ctx && ctx->valid) {
			tasks_created++;
		}
	}

	printf("  Created %d tasks\n", tasks_created);
	assert(tasks_created == MAX_TEST_TASKS);

	/* Simulate running and stopping */
	int deadline_misses = 0;
	for (int i = 0; i < MAX_TEST_TASKS; i++) {
		uint32_t pid = i + 1;
		uint64_t enqueue_time = now + (i * 1000);

		/* Some tasks will miss deadline */
		uint64_t run_delay = (i % 3 == 0) ? 200 * NSEC_PER_MSEC : 5 * NSEC_PER_MSEC;
		sim_running(pid, enqueue_time + run_delay);

		uint64_t exec_time = 10 * NSEC_PER_MSEC;
		uint64_t stop_time = enqueue_time + run_delay + exec_time;
		uint64_t cgroup = (i % 50) + 1000;

		sim_stopping(pid, cgroup, stop_time, 0);
	}

	deadline_misses = event_log_count;
	printf("  Processed %d tasks, %d deadline misses\n",
	       MAX_TEST_TASKS, deadline_misses);

	/* Roughly 1/3 should miss (those with 200ms delay vs 10-100ms budget) */
	assert(deadline_misses > 0);
	assert(deadline_misses < MAX_TEST_TASKS);

	printf("OK Stress scenario test passed\n");
}

/* Test SLO configuration updates */
static void test_slo_config_updates(void)
{
	printf("Testing SLO configuration updates...\n");
	init_simulation();

	uint64_t cgroup_id = 8000;

	/* Initial config */
	struct slo_cfg cfg = {
		.budget_ns = 100 * NSEC_PER_MSEC,
		.importance = 50,
		.flags = 0
	};
	update_slo_cfg(cgroup_id, &cfg);

	struct slo_cfg *retrieved = lookup_slo_cfg(cgroup_id);
	assert(retrieved != NULL);
	assert(retrieved->budget_ns == 100 * NSEC_PER_MSEC);
	printf("  Initial config: budget=100ms, importance=50\n");

	/* Update config */
	cfg.budget_ns = 50 * NSEC_PER_MSEC;
	cfg.importance = 90;
	update_slo_cfg(cgroup_id, &cfg);

	retrieved = lookup_slo_cfg(cgroup_id);
	assert(retrieved != NULL);
	assert(retrieved->budget_ns == 50 * NSEC_PER_MSEC);
	assert(retrieved->importance == 90);
	printf("  Updated config: budget=50ms, importance=90\n");

	/* Verify tasks use new config */
	uint64_t now = NSEC_PER_SEC;
	sim_enqueue(9001, cgroup_id, now);

	struct slo_task_ctx *ctx = get_task_ctx(9001);
	assert(ctx->budget_ns == 50 * NSEC_PER_MSEC);
	printf("  New task uses updated config\n");

	printf("OK SLO configuration updates test passed\n");
}

/* Test boundary conditions - map capacity */
static void test_map_capacity(void)
{
	printf("Testing map capacity boundaries...\n");
	init_simulation();

	/* Fill cgroup map to capacity */
	for (int i = 0; i < MAX_TEST_CGROUPS; i++) {
		struct slo_cfg cfg = {
			.budget_ns = DEFAULT_BUDGET_NS,
			.importance = 50,
			.flags = 0
		};
		int ret = update_slo_cfg(i + 1, &cfg);
		assert(ret == 0);
	}
	printf("  Filled %d cgroup entries\n", MAX_TEST_CGROUPS);

	/* Attempt to add one more - should fail */
	struct slo_cfg cfg = {
		.budget_ns = DEFAULT_BUDGET_NS,
		.importance = 50,
		.flags = 0
	};
	int ret = update_slo_cfg(MAX_TEST_CGROUPS + 1, &cfg);
	assert(ret == -ENOMEM);
	printf("  Correctly rejected entry beyond capacity\n");

	printf("OK Map capacity boundaries test passed\n");
}

/* Test event logging consistency */
static void test_event_logging_consistency(void)
{
	printf("Testing event logging consistency...\n");
	init_simulation();

	/* Generate multiple deadline misses */
	uint64_t cgroup_id = 6000;
	struct slo_cfg cfg = {
		.budget_ns = 10 * NSEC_PER_MSEC,  /* Very short */
		.importance = 99,
		.flags = 0
	};
	update_slo_cfg(cgroup_id, &cfg);

	uint64_t now = NSEC_PER_SEC;

	for (int i = 0; i < 100; i++) {
		uint32_t pid = 7000 + i;
		sim_enqueue(pid, cgroup_id, now);
		sim_running(pid, now + 50 * NSEC_PER_MSEC);  /* 50ms delay > 10ms budget */
		sim_stopping(pid, cgroup_id, now + 55 * NSEC_PER_MSEC, 0);
		now += NSEC_PER_MSEC;  /* Advance time */
	}

	assert(event_log_count == 100);
	printf("  Logged %d deadline miss events\n", event_log_count);

	/* Verify all events are consistent */
	for (int i = 0; i < 100; i++) {
		assert(event_log[i].cgroup_id == cgroup_id);
		assert(event_log[i].deadline_miss_ns > 0);
		assert(event_log[i].timestamp > 0);
	}
	printf("  All events have valid data\n");

	printf("OK Event logging consistency test passed\n");
}

int main(void)
{
	printf("Running integration tests...\n\n");

	test_basic_workflow();
	test_deadline_miss_scenario();
	test_multi_task_multi_slo();
	test_default_budget_fallback();
	test_task_lifecycle_cycles();
	test_stress_many_tasks();
	test_slo_config_updates();
	test_map_capacity();
	test_event_logging_consistency();

	printf("\nAll integration tests passed!\n");
	return 0;
}
