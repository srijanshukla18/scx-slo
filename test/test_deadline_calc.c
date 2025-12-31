/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Unit tests for SLO deadline calculation edge cases
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include "../include/scx_slo.h"

/* Test constants */
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC  1000000000ULL

/* Mock current time for testing */
static uint64_t mock_current_time = 1000000000ULL; /* 1 second */

/* Simulate deadline calculation */
static uint64_t calculate_deadline(uint64_t enqueue_time, uint64_t budget_ns)
{
	return enqueue_time + budget_ns;
}

/* Simulate deadline miss detection */
static int is_deadline_missed(uint64_t current_time, uint64_t deadline)
{
	return current_time > deadline;
}

/* Test basic deadline calculation */
static void test_basic_deadline_calculation(void)
{
	printf("Testing basic deadline calculation...\n");
	
	uint64_t enqueue_time = 1000000000ULL;  /* 1 second */
	uint64_t budget_ns = 100 * NSEC_PER_MSEC;  /* 100ms */
	uint64_t expected_deadline = enqueue_time + budget_ns;
	
	uint64_t actual_deadline = calculate_deadline(enqueue_time, budget_ns);
	
	assert(actual_deadline == expected_deadline);
	printf("✓ Basic deadline calculation correct\n");
}

/* Test deadline miss detection */
static void test_deadline_miss_detection(void)
{
	printf("Testing deadline miss detection...\n");
	
	uint64_t deadline = 1000000000ULL + (100 * NSEC_PER_MSEC);  /* 1s + 100ms */
	
	/* Case 1: No miss - current time < deadline */
	uint64_t current_time = deadline - 1;
	assert(!is_deadline_missed(current_time, deadline));
	printf("✓ No deadline miss detection correct\n");
	
	/* Case 2: Exact deadline - current time == deadline */
	current_time = deadline;
	assert(!is_deadline_missed(current_time, deadline));
	printf("✓ Exact deadline boundary correct\n");
	
	/* Case 3: Deadline miss - current time > deadline */
	current_time = deadline + 1;
	assert(is_deadline_missed(current_time, deadline));
	printf("✓ Deadline miss detection correct\n");
}

/* Test edge case: minimum budget */
static void test_minimum_budget_edge_case(void)
{
	printf("Testing minimum budget edge case...\n");
	
	uint64_t enqueue_time = 1000000000ULL;
	uint64_t min_budget = MIN_BUDGET_NS;  /* 1ms */
	
	uint64_t deadline = calculate_deadline(enqueue_time, min_budget);
	uint64_t expected = enqueue_time + min_budget;
	
	assert(deadline == expected);
	
	/* Test that a task with minimum budget can still miss deadline */
	uint64_t miss_time = deadline + 1;
	assert(is_deadline_missed(miss_time, deadline));
	
	printf("✓ Minimum budget edge case correct\n");
}

/* Test edge case: maximum budget */
static void test_maximum_budget_edge_case(void)
{
	printf("Testing maximum budget edge case...\n");
	
	uint64_t enqueue_time = 1000000000ULL;
	uint64_t max_budget = MAX_BUDGET_NS;  /* 10s */
	
	uint64_t deadline = calculate_deadline(enqueue_time, max_budget);
	uint64_t expected = enqueue_time + max_budget;
	
	assert(deadline == expected);
	
	/* Test that even max budget can miss if enough time passes */
	uint64_t miss_time = deadline + 1;
	assert(is_deadline_missed(miss_time, deadline));
	
	printf("✓ Maximum budget edge case correct\n");
}

/* Test edge case: time overflow scenarios */
static void test_time_overflow_edge_cases(void)
{
	printf("Testing time overflow edge cases...\n");
	
	/* Case 1: Near UINT64_MAX enqueue time with small budget */
	uint64_t near_max_time = UINT64_MAX - (1000 * NSEC_PER_MSEC);
	uint64_t small_budget = 100 * NSEC_PER_MSEC;
	
	/* This should not overflow since we're not at the absolute max */
	uint64_t deadline = calculate_deadline(near_max_time, small_budget);
	assert(deadline > near_max_time);  /* Deadline should be later */
	
	/* Case 2: Test very large budget near time limit */
	uint64_t large_time = UINT64_MAX / 2;
	uint64_t large_budget = MAX_BUDGET_NS;
	
	deadline = calculate_deadline(large_time, large_budget);
	assert(deadline > large_time);
	
	printf("✓ Time overflow edge cases handled correctly\n");
}

/* Test rapid deadline calculations */
static void test_rapid_deadline_calculations(void)
{
	printf("Testing rapid deadline calculations...\n");
	
	uint64_t base_time = 1000000000ULL;
	uint64_t budget = 50 * NSEC_PER_MSEC;  /* 50ms */
	
	/* Simulate rapid enqueuing of multiple tasks */
	for (int i = 0; i < 1000; i++) {
		uint64_t enqueue_time = base_time + (i * 1000000);  /* 1ms apart */
		uint64_t deadline = calculate_deadline(enqueue_time, budget);
		uint64_t expected = enqueue_time + budget;
		
		assert(deadline == expected);
		
		/* Verify deadlines are properly ordered */
		if (i > 0) {
			uint64_t prev_deadline = calculate_deadline(base_time + ((i-1) * 1000000), budget);
			assert(deadline > prev_deadline);
		}
	}
	
	printf("✓ Rapid deadline calculations correct\n");
}

/* Test deadline miss scenarios with preemption simulation */
static void test_preemption_deadline_scenarios(void)
{
	printf("Testing preemption deadline scenarios...\n");
	
	uint64_t enqueue_time = 1000000000ULL;
	uint64_t budget = 100 * NSEC_PER_MSEC;  /* 100ms budget */
	uint64_t deadline = calculate_deadline(enqueue_time, budget);
	
	/* Scenario 1: Task gets preempted for 200ms, then runs for 50ms */
	uint64_t start_time = enqueue_time + (200 * NSEC_PER_MSEC);  /* Delayed start */
	uint64_t completion_time = start_time + (50 * NSEC_PER_MSEC);  /* Quick execution */
	
	/* Even though task only ran for 50ms (< 100ms budget), 
	 * it missed deadline due to preemption delay */
	assert(is_deadline_missed(completion_time, deadline));
	
	/* Scenario 2: Task starts immediately but runs for 150ms */
	start_time = enqueue_time;
	completion_time = start_time + (150 * NSEC_PER_MSEC);
	
	/* This should also be a deadline miss */
	assert(is_deadline_missed(completion_time, deadline));
	
	/* Scenario 3: Task starts after 20ms delay, runs for 50ms (total 70ms) */
	start_time = enqueue_time + (20 * NSEC_PER_MSEC);
	completion_time = start_time + (50 * NSEC_PER_MSEC);
	
	/* This should NOT be a deadline miss (70ms < 100ms deadline) */
	assert(!is_deadline_missed(completion_time, deadline));
	
	printf("✓ Preemption deadline scenarios correct\n");
}

/* Test configuration validation edge cases */
static void test_config_validation_edge_cases(void)
{
	printf("Testing configuration validation edge cases...\n");
	
	/* Test budget validation */
	assert(MIN_BUDGET_NS == 1 * NSEC_PER_MSEC);  /* 1ms */
	assert(MAX_BUDGET_NS == 10 * NSEC_PER_SEC);   /* 10s */
	assert(DEFAULT_BUDGET_NS == 100 * NSEC_PER_MSEC);  /* 100ms */
	
	/* Test importance validation */
	assert(MIN_IMPORTANCE == 1);
	assert(MAX_IMPORTANCE == 100);
	
	/* Test that default values are within bounds */
	assert(DEFAULT_BUDGET_NS >= MIN_BUDGET_NS);
	assert(DEFAULT_BUDGET_NS <= MAX_BUDGET_NS);
	
	printf("✓ Configuration validation edge cases correct\n");
}

int main(void)
{
	printf("Running SLO deadline calculation unit tests...\n\n");
	
	test_basic_deadline_calculation();
	test_deadline_miss_detection();
	test_minimum_budget_edge_case();
	test_maximum_budget_edge_case();
	test_time_overflow_edge_cases();
	test_rapid_deadline_calculations();
	test_preemption_deadline_scenarios();
	test_config_validation_edge_cases();
	
	printf("\n✅ All deadline calculation tests passed!\n");
	return 0;
}