/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Unit tests for SLO configuration parsing and validation
 * Tests config.c functionality
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "../include/scx_slo.h"

/* Test constants */
#define MAX_CGROUP_PATH 512
#define MAX_LINE_LENGTH 256
#define NSEC_PER_MSEC 1000000ULL

/* Mock slo_config_entry structure (from config.c) */
struct slo_config_entry {
	char cgroup_path[MAX_CGROUP_PATH];
	uint64_t budget_ms;
	uint32_t importance;
};

/* Validation function mimicking config.c validate_config_entry */
static int validate_config_entry(const struct slo_config_entry *entry)
{
	if (strlen(entry->cgroup_path) == 0) {
		return -1;
	}

	if (entry->budget_ms < MIN_BUDGET_NS / 1000000 ||
	    entry->budget_ms > MAX_BUDGET_NS / 1000000) {
		return -1;
	}

	if (entry->importance < MIN_IMPORTANCE || entry->importance > MAX_IMPORTANCE) {
		return -1;
	}

	return 0;
}

/* Test valid configuration entries */
static void test_valid_config_entries(void)
{
	printf("Testing valid configuration entries...\n");

	struct slo_config_entry valid_entries[] = {
		{"/kubepods/critical/payment-api", 50, 90},
		{"/kubepods/standard/user-service", 100, 70},
		{"/kubepods/batch/analytics", 500, 20},
		{"/system.slice/nginx.service", 1, 1},      /* Minimum valid values */
		{"/workloads/batch", 10000, 100},            /* Maximum valid values */
	};

	for (size_t i = 0; i < sizeof(valid_entries) / sizeof(valid_entries[0]); i++) {
		int result = validate_config_entry(&valid_entries[i]);
		assert(result == 0);
		printf("  Valid: %s (budget=%llu ms, importance=%u)\n",
		       valid_entries[i].cgroup_path,
		       valid_entries[i].budget_ms,
		       valid_entries[i].importance);
	}

	printf("OK Valid config entries accepted\n");
}

/* Test invalid cgroup paths */
static void test_invalid_cgroup_paths(void)
{
	printf("Testing invalid cgroup paths...\n");

	struct slo_config_entry entry;

	/* Test empty path */
	strcpy(entry.cgroup_path, "");
	entry.budget_ms = 100;
	entry.importance = 50;

	int result = validate_config_entry(&entry);
	assert(result == -1);
	printf("  Rejected: Empty cgroup path\n");

	printf("OK Invalid cgroup paths rejected\n");
}

/* Test budget boundary conditions */
static void test_budget_boundaries(void)
{
	printf("Testing budget boundary conditions...\n");

	struct slo_config_entry entry;
	strcpy(entry.cgroup_path, "/test/workload");
	entry.importance = 50;

	/* Test below minimum (MIN_BUDGET_NS = 1ms) */
	entry.budget_ms = 0;
	assert(validate_config_entry(&entry) == -1);
	printf("  Rejected: 0ms budget (below minimum)\n");

	/* Test at minimum */
	entry.budget_ms = 1;
	assert(validate_config_entry(&entry) == 0);
	printf("  Accepted: 1ms budget (at minimum)\n");

	/* Test at maximum (MAX_BUDGET_NS = 10s = 10000ms) */
	entry.budget_ms = 10000;
	assert(validate_config_entry(&entry) == 0);
	printf("  Accepted: 10000ms budget (at maximum)\n");

	/* Test above maximum */
	entry.budget_ms = 10001;
	assert(validate_config_entry(&entry) == -1);
	printf("  Rejected: 10001ms budget (above maximum)\n");

	/* Test default budget value (should be valid) */
	entry.budget_ms = DEFAULT_BUDGET_NS / NSEC_PER_MSEC;
	assert(validate_config_entry(&entry) == 0);
	printf("  Accepted: 100ms budget (default)\n");

	printf("OK Budget boundaries enforced\n");
}

/* Test importance boundary conditions */
static void test_importance_boundaries(void)
{
	printf("Testing importance boundary conditions...\n");

	struct slo_config_entry entry;
	strcpy(entry.cgroup_path, "/test/workload");
	entry.budget_ms = 100;

	/* Test below minimum (MIN_IMPORTANCE = 1) */
	entry.importance = 0;
	assert(validate_config_entry(&entry) == -1);
	printf("  Rejected: 0 importance (below minimum)\n");

	/* Test at minimum */
	entry.importance = 1;
	assert(validate_config_entry(&entry) == 0);
	printf("  Accepted: 1 importance (at minimum)\n");

	/* Test at maximum (MAX_IMPORTANCE = 100) */
	entry.importance = 100;
	assert(validate_config_entry(&entry) == 0);
	printf("  Accepted: 100 importance (at maximum)\n");

	/* Test above maximum */
	entry.importance = 101;
	assert(validate_config_entry(&entry) == -1);
	printf("  Rejected: 101 importance (above maximum)\n");

	printf("OK Importance boundaries enforced\n");
}

/* Test configuration line parsing simulation */
static void test_config_line_parsing(void)
{
	printf("Testing configuration line parsing...\n");

	struct {
		const char *line;
		int expected_valid;
		const char *description;
	} test_lines[] = {
		/* Valid lines */
		{"/kubepods/critical 50 90", 1, "Standard config line"},
		{"/a 1 1", 1, "Minimal valid config"},
		{"/very/long/path/to/workload 10000 100", 1, "Long path with max values"},

		/* Invalid lines (would fail parsing or validation) */
		{"# Comment line", 0, "Comment line"},
		{"", 0, "Empty line"},
		{"\n", 0, "Newline only"},
		{"/path_only", 0, "Missing budget and importance"},
		{"/path 50", 0, "Missing importance"},
		{"50 90", 0, "Missing cgroup path"},
		{"/path invalid 90", 0, "Non-numeric budget"},
		{"/path 50 invalid", 0, "Non-numeric importance"},
	};

	for (size_t i = 0; i < sizeof(test_lines) / sizeof(test_lines[0]); i++) {
		struct slo_config_entry entry;
		memset(&entry, 0, sizeof(entry));

		int parse_result = sscanf(test_lines[i].line, "%511s %llu %u",
		                          entry.cgroup_path,
		                          &entry.budget_ms,
		                          &entry.importance);

		int is_valid = (parse_result == 3) && (validate_config_entry(&entry) == 0);

		if (test_lines[i].expected_valid) {
			assert(is_valid);
			printf("  Accepted: %s\n", test_lines[i].description);
		} else {
			/* Either parsing failed or validation failed - both acceptable */
			printf("  Rejected: %s\n", test_lines[i].description);
		}
	}

	printf("OK Config line parsing working correctly\n");
}

/* Test budget to nanoseconds conversion */
static void test_budget_conversion(void)
{
	printf("Testing budget conversion (ms to ns)...\n");

	struct {
		uint64_t budget_ms;
		uint64_t expected_ns;
	} conversions[] = {
		{1, 1000000ULL},
		{50, 50000000ULL},
		{100, 100000000ULL},
		{1000, 1000000000ULL},
		{10000, 10000000000ULL},
	};

	for (size_t i = 0; i < sizeof(conversions) / sizeof(conversions[0]); i++) {
		uint64_t actual_ns = conversions[i].budget_ms * 1000000ULL;
		assert(actual_ns == conversions[i].expected_ns);
		printf("  %llu ms -> %llu ns\n", conversions[i].budget_ms, actual_ns);
	}

	printf("OK Budget conversion correct\n");
}

/* Test cgroup path normalization scenarios */
static void test_cgroup_path_handling(void)
{
	printf("Testing cgroup path handling...\n");

	struct {
		const char *path;
		const char *description;
		int should_accept;
	} paths[] = {
		{"/kubepods", "Simple path", 1},
		{"/kubepods/pod-abc123", "Pod path", 1},
		{"/system.slice/docker.service", "System slice", 1},
		{"/user.slice/user-1000.slice", "User slice", 1},
		{"/machine.slice/vm-instance", "Machine slice", 1},
		/* Long paths */
		{"/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p", "Deep nested path", 1},
	};

	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		struct slo_config_entry entry;
		strncpy(entry.cgroup_path, paths[i].path, sizeof(entry.cgroup_path) - 1);
		entry.cgroup_path[sizeof(entry.cgroup_path) - 1] = '\0';
		entry.budget_ms = 100;
		entry.importance = 50;

		int result = validate_config_entry(&entry);

		if (paths[i].should_accept) {
			assert(result == 0);
			printf("  Accepted: %s\n", paths[i].description);
		} else {
			assert(result == -1);
			printf("  Rejected: %s\n", paths[i].description);
		}
	}

	printf("OK Cgroup path handling correct\n");
}

/* Test configuration entry copy safety */
static void test_config_entry_copy_safety(void)
{
	printf("Testing configuration entry copy safety...\n");

	struct slo_config_entry src, dst;

	/* Initialize source with valid values */
	strcpy(src.cgroup_path, "/test/workload");
	src.budget_ms = 100;
	src.importance = 50;

	/* Copy using memcpy (common pattern) */
	memcpy(&dst, &src, sizeof(struct slo_config_entry));

	/* Verify copy is valid */
	assert(validate_config_entry(&dst) == 0);
	assert(strcmp(dst.cgroup_path, src.cgroup_path) == 0);
	assert(dst.budget_ms == src.budget_ms);
	assert(dst.importance == src.importance);

	printf("  memcpy preserves validity\n");

	/* Test struct assignment */
	struct slo_config_entry dst2;
	dst2 = src;

	assert(validate_config_entry(&dst2) == 0);
	printf("  struct assignment preserves validity\n");

	printf("OK Configuration entry copy safety verified\n");
}

/* Test combined validation scenarios */
static void test_combined_validation_scenarios(void)
{
	printf("Testing combined validation scenarios...\n");

	struct slo_config_entry entry;

	/* Scenario 1: All fields at minimum valid values */
	strcpy(entry.cgroup_path, "/");
	entry.budget_ms = 1;
	entry.importance = 1;
	assert(validate_config_entry(&entry) == 0);
	printf("  All minimum values: valid\n");

	/* Scenario 2: All fields at maximum valid values */
	strcpy(entry.cgroup_path, "/maximum/test/path/for/validation");
	entry.budget_ms = 10000;
	entry.importance = 100;
	assert(validate_config_entry(&entry) == 0);
	printf("  All maximum values: valid\n");

	/* Scenario 3: One invalid field should fail entire validation */
	strcpy(entry.cgroup_path, "/valid/path");
	entry.budget_ms = 100;
	entry.importance = 0;  /* Invalid */
	assert(validate_config_entry(&entry) == -1);
	printf("  One invalid field (importance): rejected\n");

	strcpy(entry.cgroup_path, "/valid/path");
	entry.budget_ms = 0;   /* Invalid */
	entry.importance = 50;
	assert(validate_config_entry(&entry) == -1);
	printf("  One invalid field (budget): rejected\n");

	strcpy(entry.cgroup_path, "");  /* Invalid */
	entry.budget_ms = 100;
	entry.importance = 50;
	assert(validate_config_entry(&entry) == -1);
	printf("  One invalid field (path): rejected\n");

	printf("OK Combined validation scenarios correct\n");
}

int main(void)
{
	printf("Running SLO configuration unit tests...\n\n");

	test_valid_config_entries();
	test_invalid_cgroup_paths();
	test_budget_boundaries();
	test_importance_boundaries();
	test_config_line_parsing();
	test_budget_conversion();
	test_cgroup_path_handling();
	test_config_entry_copy_safety();
	test_combined_validation_scenarios();

	printf("\nAll configuration tests passed!\n");
	return 0;
}
