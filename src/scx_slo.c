/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <libgen.h>
#include <errno.h>
#include <time.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <scx/common.h>
#include "scx_slo.skel.h"
#include "config.h"

/* Deadline event structure - must match BPF side */
struct deadline_event {
	__u64 cgroup_id;
	__u64 deadline_miss_ns;
	__u64 timestamp;
};

const char help_fmt[] =
"SLO-aware sched_ext scheduler (scx-slo).\n"
"\n"
"Enforces service-level latency budgets at the kernel level.\n"
"\n"
"Usage: %s [-v] [-c] [--create-config]\n"
"\n"
"  -v            Print libbpf debug messages and detailed deadline events\n"
"  -c            Reload configuration file on startup\n"
"  --create-config Create example configuration file\n"
"  -h            Display this help and exit\n"
"\n"
"Configuration:\n"
"  Default config: /etc/scx-slo/config\n"
"  Format: cgroup_path budget_ms importance\n"
"  Example: /kubepods/critical/payment-api 50 90\n";

static bool verbose;
static bool reload_config;
static volatile int exit_req;

/* Statistics for deadline misses */
static __u64 total_deadline_misses = 0;
static __u64 total_miss_duration_ns = 0;

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sigint_handler(int sig)
{
	printf("\nReceived signal %d, shutting down gracefully...\n", sig);
	exit_req = 1;
}

/* Convert nanoseconds to milliseconds for readable output */
static double ns_to_ms(u64 ns)
{
	return (double)ns / 1000000.0;
}

/* Ring buffer callback for deadline miss events */
static int handle_deadline_event(void *ctx, void *data, size_t data_sz)
{
	const struct deadline_event *event = data;
	
	if (data_sz < sizeof(*event)) {
		fprintf(stderr, "Invalid event size: %zu\n", data_sz);
		return 0;
	}
	
	total_deadline_misses++;
	total_miss_duration_ns += event->deadline_miss_ns;
	
	if (verbose) {
		printf("DEADLINE MISS: cgroup=%llu, miss=%.2fms, time=%llu\n",
		       event->cgroup_id, 
		       ns_to_ms(event->deadline_miss_ns),
		       event->timestamp);
	}
	
	return 0;
}

static void read_stats(struct scx_slo *skel, __u64 *stats)
{
	int nr_cpus = libbpf_num_possible_cpus();
	assert(nr_cpus > 0);
	__u64 cnts[2][nr_cpus];
	__u32 idx;

	memset(stats, 0, sizeof(stats[0]) * 2);

	for (idx = 0; idx < 2; idx++) {
		int ret, cpu;

		ret = bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats),
					  &idx, cnts[idx]);
		if (ret < 0)
			continue;
		for (cpu = 0; cpu < nr_cpus; cpu++)
			stats[idx] += cnts[idx][cpu];
	}
}

int main(int argc, char **argv)
{
	struct scx_slo *skel;
	struct bpf_link *link;
	struct ring_buffer *rb = NULL;
	__u32 opt;
	__u64 ecode;
	int err;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
	signal(SIGPIPE, SIG_IGN);  /* Ignore broken pipe */
restart:
	skel = SCX_OPS_OPEN(slo_ops, scx_slo);

	/* Handle --create-config first */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--create-config") == 0) {
			return create_example_config() == 0 ? 0 : 1;
		}
	}

	while ((opt = getopt(argc, argv, "vch")) != -1) {
		switch (opt) {
		case 'v':
			verbose = true;
			break;
		case 'c':
			reload_config = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}

	err = SCX_OPS_LOAD(skel, slo_ops, scx_slo, uei);
	if (err) {
		fprintf(stderr, "Failed to load BPF program: %d\n", err);
		goto cleanup;
	}

	link = SCX_OPS_ATTACH(skel, slo_ops, scx_slo);
	if (!link) {
		fprintf(stderr, "Failed to attach BPF program\n");
		goto cleanup;
	}

	/* Set up ring buffer for deadline events */
	rb = ring_buffer__new(bpf_map__fd(skel->maps.deadline_events), 
			      handle_deadline_event, NULL, NULL);
	if (!rb) {
		fprintf(stderr, "Failed to create ring buffer\n");
		goto cleanup;
	}

	/* Load SLO configuration if requested */
	if (reload_config) {
		int config_entries = load_slo_config(bpf_map__fd(skel->maps.slo_map));
		if (config_entries < 0) {
			fprintf(stderr, "Failed to load configuration\n");
			goto cleanup;
		}
	}

	printf("SLO scheduler started. Press Ctrl-C to exit.\n");
	if (verbose) {
		printf("Verbose mode: showing detailed deadline miss events\n");
	}

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		__u64 stats[2];

		/* Poll ring buffer for deadline events */
		err = ring_buffer__poll(rb, 100);  /* 100ms timeout */
		if (err < 0 && err != -EINTR) {
			fprintf(stderr, "Error polling ring buffer: %d\n", err);
			break;
		}

		read_stats(skel, stats);
		
		/* Enhanced stats with deadline miss information */
		printf("local=%llu global=%llu deadline_misses=%llu avg_miss=%.2fms\n", 
		       stats[0], stats[1], total_deadline_misses,
		       total_deadline_misses > 0 ? ns_to_ms(total_miss_duration_ns / total_deadline_misses) : 0.0);
		fflush(stdout);
		sleep(1);
	}

cleanup:
	printf("Cleaning up SLO scheduler...\n");
	
	if (rb) {
		printf("Freeing ring buffer...\n");
		ring_buffer__free(rb);
	}
	
	if (link) {
		printf("Detaching BPF program...\n");
		bpf_link__destroy(link);
	}
	
	if (skel) {
		ecode = UEI_REPORT(skel, uei);
		scx_slo__destroy(skel);
		printf("BPF scheduler detached successfully\n");

		if (UEI_ECODE_RESTART(ecode))
			goto restart;
	}
	
	/* Print final statistics */
	if (total_deadline_misses > 0) {
		printf("Final stats: %llu deadline misses, avg miss %.2fms\n",
		       total_deadline_misses,
		       ns_to_ms(total_miss_duration_ns / total_deadline_misses));
	} else {
		printf("Final stats: No deadline misses detected\n");
	}
	
	return err;
}
