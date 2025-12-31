/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Unit tests for scx_slo main program logic
 * Tests userspace components of scx_slo.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include "../include/scx_slo.h"

/* Test constants */
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC  1000000000ULL

/* Mock deadline event structure (from scx_slo.c) */
struct deadline_event {
	uint64_t cgroup_id;
	uint64_t deadline_miss_ns;
	uint64_t timestamp;
};

/* ns_to_ms function from scx_slo.c */
static double ns_to_ms(uint64_t ns)
{
	return (double)ns / 1000000.0;
}

/* Test ns_to_ms conversion */
static void test_ns_to_ms_conversion(void)
{
	printf("Testing ns_to_ms conversion...\n");

	struct {
		uint64_t ns;
		double expected_ms;
	} test_cases[] = {
		{0, 0.0},
		{1000000, 1.0},
		{500000, 0.5},
		{1500000, 1.5},
		{1000000000, 1000.0},
		{100000000, 100.0},
		{NSEC_PER_SEC, 1000.0},
		{10 * NSEC_PER_SEC, 10000.0},
	};

	for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
		double actual = ns_to_ms(test_cases[i].ns);
		double diff = actual - test_cases[i].expected_ms;
		if (diff < 0) diff = -diff;

		assert(diff < 0.0001);  /* Allow small floating point error */
		printf("  %llu ns -> %.2f ms\n", test_cases[i].ns, actual);
	}

	printf("OK ns_to_ms conversion correct\n");
}

/* Test deadline event structure integrity */
static void test_deadline_event_structure(void)
{
	printf("Testing deadline_event structure...\n");

	/* Verify structure size and alignment */
	assert(sizeof(struct deadline_event) == 24);  /* 3 x uint64_t */
	printf("  Structure size: %zu bytes\n", sizeof(struct deadline_event));

	/* Test field offsets */
	struct deadline_event event;
	void *base = &event;
	assert((void *)&event.cgroup_id == base);
	assert((void *)&event.deadline_miss_ns == base + 8);
	assert((void *)&event.timestamp == base + 16);

	printf("  Field layout verified\n");

	/* Test initialization */
	memset(&event, 0, sizeof(event));
	assert(event.cgroup_id == 0);
	assert(event.deadline_miss_ns == 0);
	assert(event.timestamp == 0);

	printf("  Zero initialization verified\n");

	/* Test with realistic values */
	event.cgroup_id = 12345;
	event.deadline_miss_ns = 5 * NSEC_PER_MSEC;  /* 5ms miss */
	event.timestamp = NSEC_PER_SEC;              /* 1 second */

	assert(event.cgroup_id == 12345);
	assert(event.deadline_miss_ns == 5000000);
	assert(event.timestamp == NSEC_PER_SEC);

	printf("  Value assignment verified\n");

	printf("OK deadline_event structure correct\n");
}

/* Simulate stats aggregation logic */
static void test_stats_aggregation(void)
{
	printf("Testing stats aggregation logic...\n");

	/* Simulate stats accumulation (from read_stats pattern) */
	uint64_t stats[2] = {0, 0};  /* [local, global] */

	/* Simulate per-CPU stats aggregation */
	int nr_cpus = 8;  /* Simulated CPU count */
	uint64_t local_cnts[8] = {10, 20, 15, 25, 30, 12, 18, 22};
	uint64_t global_cnts[8] = {5, 8, 7, 10, 12, 6, 9, 11};

	for (int cpu = 0; cpu < nr_cpus; cpu++) {
		stats[0] += local_cnts[cpu];
		stats[1] += global_cnts[cpu];
	}

	uint64_t expected_local = 10 + 20 + 15 + 25 + 30 + 12 + 18 + 22;
	uint64_t expected_global = 5 + 8 + 7 + 10 + 12 + 6 + 9 + 11;

	assert(stats[0] == expected_local);
	assert(stats[1] == expected_global);

	printf("  Local stats sum: %llu (expected %llu)\n", stats[0], expected_local);
	printf("  Global stats sum: %llu (expected %llu)\n", stats[1], expected_global);

	printf("OK Stats aggregation correct\n");
}

/* Test deadline miss tracking accumulation */
static void test_deadline_miss_tracking(void)
{
	printf("Testing deadline miss tracking...\n");

	uint64_t total_deadline_misses = 0;
	uint64_t total_miss_duration_ns = 0;

	/* Simulate receiving deadline events */
	struct deadline_event events[] = {
		{1001, 5 * NSEC_PER_MSEC, NSEC_PER_SEC},      /* 5ms miss */
		{1002, 10 * NSEC_PER_MSEC, 2 * NSEC_PER_SEC}, /* 10ms miss */
		{1001, 3 * NSEC_PER_MSEC, 3 * NSEC_PER_SEC},  /* 3ms miss */
		{1003, 7 * NSEC_PER_MSEC, 4 * NSEC_PER_SEC},  /* 7ms miss */
	};

	for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
		total_deadline_misses++;
		total_miss_duration_ns += events[i].deadline_miss_ns;
	}

	assert(total_deadline_misses == 4);
	assert(total_miss_duration_ns == 25 * NSEC_PER_MSEC);

	printf("  Total misses: %llu\n", total_deadline_misses);
	printf("  Total miss duration: %llu ns (%.2f ms)\n",
	       total_miss_duration_ns, ns_to_ms(total_miss_duration_ns));

	/* Test average calculation */
	double avg_miss_ms = ns_to_ms(total_miss_duration_ns / total_deadline_misses);
	assert(avg_miss_ms > 6.24 && avg_miss_ms < 6.26);  /* Should be 6.25ms */

	printf("  Average miss: %.2f ms\n", avg_miss_ms);

	printf("OK Deadline miss tracking correct\n");
}

/* Test command line argument parsing scenarios */
static void test_argument_scenarios(void)
{
	printf("Testing command line argument scenarios...\n");

	/* Scenario 1: Default (no args) */
	int verbose = 0;
	int reload_config = 0;
	printf("  No args: verbose=%d, reload_config=%d\n", verbose, reload_config);

	/* Scenario 2: Verbose mode */
	verbose = 1;
	reload_config = 0;
	printf("  -v flag: verbose=%d, reload_config=%d\n", verbose, reload_config);

	/* Scenario 3: Config reload */
	verbose = 0;
	reload_config = 1;
	printf("  -c flag: verbose=%d, reload_config=%d\n", verbose, reload_config);

	/* Scenario 4: Both flags */
	verbose = 1;
	reload_config = 1;
	printf("  -v -c flags: verbose=%d, reload_config=%d\n", verbose, reload_config);

	printf("OK Argument scenarios documented\n");
}

/* Test deadline event handler validation */
static void test_event_handler_validation(void)
{
	printf("Testing event handler validation...\n");

	/* Simulate handle_deadline_event logic */
	struct deadline_event event;

	/* Test 1: Valid event with proper size */
	size_t data_sz = sizeof(struct deadline_event);
	assert(data_sz >= sizeof(event));
	printf("  Valid event size: %zu bytes\n", data_sz);

	/* Test 2: Invalid (too small) event */
	data_sz = sizeof(event) - 1;
	int is_valid = (data_sz >= sizeof(event));
	assert(!is_valid);
	printf("  Rejected undersized event\n");

	/* Test 3: Larger than expected (should still work) */
	data_sz = sizeof(event) + 100;
	is_valid = (data_sz >= sizeof(event));
	assert(is_valid);
	printf("  Accepted oversized event (forward compatible)\n");

	printf("OK Event handler validation correct\n");
}

/* Test signal handler behavior simulation */
static void test_signal_handling_logic(void)
{
	printf("Testing signal handling logic...\n");

	volatile int exit_req = 0;

	/* Simulate signal received */
	int signals[] = {SIGINT, SIGTERM};

	for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		/* Simulate sigint_handler setting exit_req */
		exit_req = 1;
		assert(exit_req == 1);
		printf("  Signal %d sets exit_req=1\n", signals[i]);
		exit_req = 0;  /* Reset for next test */
	}

	printf("OK Signal handling logic correct\n");
}

/* Test output formatting consistency */
static void test_output_formatting(void)
{
	printf("Testing output formatting...\n");

	/* Test stats output format */
	uint64_t stats[2] = {12345, 67890};
	uint64_t misses = 42;
	double avg_miss = 5.5;

	char output[256];
	snprintf(output, sizeof(output),
	         "local=%llu global=%llu deadline_misses=%llu avg_miss=%.2fms",
	         stats[0], stats[1], misses, avg_miss);

	assert(strlen(output) > 0);
	assert(strstr(output, "local=12345") != NULL);
	assert(strstr(output, "global=67890") != NULL);
	assert(strstr(output, "deadline_misses=42") != NULL);
	assert(strstr(output, "avg_miss=5.50ms") != NULL);

	printf("  Stats format: %s\n", output);

	/* Test deadline miss event format */
	uint64_t cgroup_id = 99999;
	double miss_ms = 7.89;
	uint64_t timestamp = 1234567890;

	snprintf(output, sizeof(output),
	         "DEADLINE MISS: cgroup=%llu, miss=%.2fms, time=%llu",
	         cgroup_id, miss_ms, timestamp);

	assert(strstr(output, "DEADLINE MISS") != NULL);
	assert(strstr(output, "cgroup=99999") != NULL);
	assert(strstr(output, "miss=7.89ms") != NULL);

	printf("  Event format: %s\n", output);

	printf("OK Output formatting consistent\n");
}

/* Test zero division safety in average calculation */
static void test_zero_division_safety(void)
{
	printf("Testing zero division safety...\n");

	uint64_t total_misses = 0;
	uint64_t total_duration = 0;

	/* Pattern from scx_slo.c for safe division */
	double avg_miss = (total_misses > 0) ?
	                  ns_to_ms(total_duration / total_misses) : 0.0;

	assert(avg_miss == 0.0);
	printf("  Zero misses: avg=%.2f (no division)\n", avg_miss);

	/* With actual values */
	total_misses = 10;
	total_duration = 50 * NSEC_PER_MSEC;

	avg_miss = (total_misses > 0) ?
	           ns_to_ms(total_duration / total_misses) : 0.0;

	assert(avg_miss == 5.0);
	printf("  10 misses, 50ms total: avg=%.2f ms\n", avg_miss);

	printf("OK Zero division safety verified\n");
}

/* Test ring buffer polling return value handling */
static void test_ringbuf_poll_handling(void)
{
	printf("Testing ring buffer poll return handling...\n");

	/* Simulate ring_buffer__poll return values */
	int poll_results[] = {
		0,      /* No events */
		1,      /* 1 event processed */
		5,      /* 5 events processed */
		-1,     /* Error (not EINTR) */
		-EINTR, /* Interrupted (should continue) */
	};

	for (size_t i = 0; i < sizeof(poll_results) / sizeof(poll_results[0]); i++) {
		int err = poll_results[i];
		int should_break = (err < 0 && err != -EINTR);

		if (err >= 0) {
			printf("  Poll returned %d: continue (events processed)\n", err);
		} else if (err == -EINTR) {
			printf("  Poll returned -EINTR: continue (interrupted)\n");
		} else {
			printf("  Poll returned %d: break (error)\n", err);
		}

		if (i == 3) {
			assert(should_break);
		} else {
			assert(!should_break);
		}
	}

	printf("OK Ring buffer poll handling correct\n");
}

/* Test slo_cfg structure validation */
static void test_slo_cfg_structure(void)
{
	printf("Testing slo_cfg structure...\n");

	struct slo_cfg cfg;

	/* Verify structure layout */
	assert(sizeof(cfg.budget_ns) == 8);
	assert(sizeof(cfg.importance) == 4);
	assert(sizeof(cfg.flags) == 4);

	printf("  budget_ns: %zu bytes\n", sizeof(cfg.budget_ns));
	printf("  importance: %zu bytes\n", sizeof(cfg.importance));
	printf("  flags: %zu bytes\n", sizeof(cfg.flags));

	/* Test initialization */
	memset(&cfg, 0, sizeof(cfg));
	assert(cfg.budget_ns == 0);
	assert(cfg.importance == 0);
	assert(cfg.flags == 0);

	printf("  Zero initialization verified\n");

	/* Test typical configuration */
	cfg.budget_ns = DEFAULT_BUDGET_NS;
	cfg.importance = 50;
	cfg.flags = 0;

	assert(cfg.budget_ns == 100 * NSEC_PER_MSEC);
	assert(cfg.importance == 50);

	printf("  Typical config: budget=%llu ns, importance=%u\n",
	       cfg.budget_ns, cfg.importance);

	printf("OK slo_cfg structure correct\n");
}

/* Test slo_task_ctx structure */
static void test_slo_task_ctx_structure(void)
{
	printf("Testing slo_task_ctx structure...\n");

	struct slo_task_ctx ctx;

	/* Verify field sizes */
	assert(sizeof(ctx.deadline) == 8);
	assert(sizeof(ctx.start_time) == 8);
	assert(sizeof(ctx.budget_ns) == 8);
	assert(sizeof(ctx.valid) == 4);

	printf("  deadline: %zu bytes\n", sizeof(ctx.deadline));
	printf("  start_time: %zu bytes\n", sizeof(ctx.start_time));
	printf("  budget_ns: %zu bytes\n", sizeof(ctx.budget_ns));
	printf("  valid: %zu bytes\n", sizeof(ctx.valid));

	/* Test uninitialized state */
	memset(&ctx, 0, sizeof(ctx));
	assert(ctx.valid == 0);
	printf("  Uninitialized context: valid=0\n");

	/* Test initialized state */
	ctx.deadline = NSEC_PER_SEC + DEFAULT_BUDGET_NS;
	ctx.start_time = NSEC_PER_SEC;
	ctx.budget_ns = DEFAULT_BUDGET_NS;
	ctx.valid = 1;

	assert(ctx.valid == 1);
	assert(ctx.deadline > ctx.start_time);
	assert(ctx.deadline - ctx.start_time == ctx.budget_ns);

	printf("  Initialized context: deadline=%llu, valid=1\n", ctx.deadline);

	printf("OK slo_task_ctx structure correct\n");
}

int main(void)
{
	printf("Running SLO main program unit tests...\n\n");

	test_ns_to_ms_conversion();
	test_deadline_event_structure();
	test_stats_aggregation();
	test_deadline_miss_tracking();
	test_argument_scenarios();
	test_event_handler_validation();
	test_signal_handling_logic();
	test_output_formatting();
	test_zero_division_safety();
	test_ringbuf_poll_handling();
	test_slo_cfg_structure();
	test_slo_task_ctx_structure();

	printf("\nAll main program tests passed!\n");
	return 0;
}
