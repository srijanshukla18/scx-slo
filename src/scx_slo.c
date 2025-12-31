/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 *
 * scx-slo userspace agent - SLO-aware sched_ext scheduler
 *
 * This program is Linux-specific and requires:
 * - Linux 6.12+ with CONFIG_SCHED_CLASS_EXT=y
 * - libbpf, libelf, zlib
 * - pthreads
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <libgen.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <scx/common.h>
#include "scx_slo.skel.h"
#include "config.h"

/* Linux-specific: MSG_NOSIGNAL for send() to prevent SIGPIPE */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0x4000
#endif

/* Deadline event structure - must match BPF side */
struct deadline_event {
	__u64 cgroup_id;
	__u64 deadline_miss_ns;
	__u64 timestamp;
};

/* Log levels */
enum log_level {
	LOG_DEBUG = 0,
	LOG_INFO = 1,
	LOG_WARN = 2,
	LOG_ERROR = 3
};

static const char *log_level_names[] = {"debug", "info", "warn", "error"};

const char help_fmt[] =
"SLO-aware sched_ext scheduler (scx-slo).\n"
"\n"
"Enforces service-level latency budgets at the kernel level.\n"
"\n"
"Usage: %s [-v] [-c] [-p PORT] [-j] [-l LEVEL] [--create-config]\n"
"\n"
"  -v            Print libbpf debug messages and detailed deadline events\n"
"  -c            Reload configuration file on startup\n"
"  -p PORT       HTTP health check port (default: 8080, 0 to disable)\n"
"  -j            Enable JSON structured logging\n"
"  -l LEVEL      Log level: debug, info, warn, error (default: info)\n"
"  --create-config Create example configuration file\n"
"  -h            Display this help and exit\n"
"\n"
"HTTP Endpoints:\n"
"  GET /health   Returns 200 if scheduler is attached\n"
"  GET /metrics  Returns Prometheus-format metrics\n"
"\n"
"Configuration:\n"
"  Default config: /etc/scx-slo/config\n"
"  Format: cgroup_path budget_ms importance\n"
"  Example: /kubepods/critical/payment-api 50 90\n";

/* Configuration */
static bool verbose;
static bool reload_config;
static bool json_logging = false;
static enum log_level current_log_level = LOG_INFO;
static int health_port = 8080;
static volatile sig_atomic_t exit_req = 0;
static volatile sig_atomic_t scheduler_attached = 0;

/* Cleanup timeout in seconds */
#define CLEANUP_TIMEOUT_SEC 5

/* Statistics - protected by stats_lock for thread safety */
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static __u64 total_deadline_misses = 0;
static __u64 total_miss_duration_ns = 0;
static __u64 last_local_dispatches = 0;
static __u64 last_global_dispatches = 0;

/* Health server state */
static int health_server_fd = -1;
static pthread_t health_thread;
static volatile bool health_thread_running = false;

/* Structured logging */
static void log_msg(enum log_level level, const char *fmt, ...)
{
	if (level < current_log_level)
		return;

	va_list args;
	va_start(args, fmt);

	time_t now = time(NULL);
	struct tm *tm_info = localtime(&now);
	char timestamp[32];
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", tm_info);

	if (json_logging) {
		char msg[1024];
		vsnprintf(msg, sizeof(msg), fmt, args);
		/* Escape quotes in message for JSON */
		char escaped_msg[2048];
		size_t j = 0;
		for (size_t i = 0; msg[i] && j < sizeof(escaped_msg) - 2; i++) {
			if (msg[i] == '"' || msg[i] == '\\') {
				escaped_msg[j++] = '\\';
			}
			escaped_msg[j++] = msg[i];
		}
		escaped_msg[j] = '\0';
		printf("{\"timestamp\":\"%s\",\"level\":\"%s\",\"message\":\"%s\"}\n",
		       timestamp, log_level_names[level], escaped_msg);
	} else {
		printf("[%s] [%s] ", timestamp, log_level_names[level]);
		vprintf(fmt, args);
		printf("\n");
	}
	va_end(args);
	fflush(stdout);
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sigint_handler(int sig)
{
	log_msg(LOG_INFO, "Received signal %d, initiating graceful shutdown", sig);
	exit_req = 1;
}

/* Convert nanoseconds to milliseconds for readable output */
static double ns_to_ms(__u64 ns)
{
	return (double)ns / 1000000.0;
}

/* HTTP response helpers */
static void send_http_response(int client_fd, int status_code, const char *status_text,
			       const char *content_type, const char *body)
{
	char response[4096];
	int body_len = body ? strlen(body) : 0;

	int len = snprintf(response, sizeof(response),
			   "HTTP/1.1 %d %s\r\n"
			   "Content-Type: %s\r\n"
			   "Content-Length: %d\r\n"
			   "Connection: close\r\n"
			   "\r\n"
			   "%s",
			   status_code, status_text, content_type, body_len,
			   body ? body : "");

	if (len > 0 && (size_t)len < sizeof(response)) {
		send(client_fd, response, len, MSG_NOSIGNAL);
	}
}

/* Health check handler */
static void handle_health_request(int client_fd)
{
	if (scheduler_attached) {
		send_http_response(client_fd, 200, "OK", "text/plain", "OK\n");
	} else {
		send_http_response(client_fd, 503, "Service Unavailable",
				   "text/plain", "Scheduler not attached\n");
	}
}

/* Prometheus metrics handler */
static void handle_metrics_request(int client_fd)
{
	char metrics[4096];
	__u64 misses, miss_duration, local, global;

	pthread_mutex_lock(&stats_lock);
	misses = total_deadline_misses;
	miss_duration = total_miss_duration_ns;
	local = last_local_dispatches;
	global = last_global_dispatches;
	pthread_mutex_unlock(&stats_lock);

	double avg_miss_ms = misses > 0 ? ns_to_ms(miss_duration / misses) : 0.0;

	int len = snprintf(metrics, sizeof(metrics),
		"# HELP scx_slo_deadline_misses_total Total number of deadline misses\n"
		"# TYPE scx_slo_deadline_misses_total counter\n"
		"scx_slo_deadline_misses_total %llu\n"
		"\n"
		"# HELP scx_slo_local_dispatches_total Tasks dispatched to local DSQ\n"
		"# TYPE scx_slo_local_dispatches_total counter\n"
		"scx_slo_local_dispatches_total %llu\n"
		"\n"
		"# HELP scx_slo_global_dispatches_total Tasks dispatched to global DSQ\n"
		"# TYPE scx_slo_global_dispatches_total counter\n"
		"scx_slo_global_dispatches_total %llu\n"
		"\n"
		"# HELP scx_slo_avg_miss_duration_seconds Average deadline miss duration\n"
		"# TYPE scx_slo_avg_miss_duration_seconds gauge\n"
		"scx_slo_avg_miss_duration_seconds %.6f\n"
		"\n"
		"# HELP scx_slo_scheduler_attached Whether scheduler is attached\n"
		"# TYPE scx_slo_scheduler_attached gauge\n"
		"scx_slo_scheduler_attached %d\n",
		(unsigned long long)misses,
		(unsigned long long)local,
		(unsigned long long)global,
		avg_miss_ms / 1000.0,  /* Convert ms to seconds */
		scheduler_attached ? 1 : 0);

	if (len > 0 && (size_t)len < sizeof(metrics)) {
		send_http_response(client_fd, 200, "OK",
				   "text/plain; version=0.0.4", metrics);
	} else {
		send_http_response(client_fd, 500, "Internal Server Error",
				   "text/plain", "Metrics buffer overflow\n");
	}
}

/* Parse HTTP request and route to handler */
static void handle_http_request(int client_fd)
{
	char buffer[1024];
	ssize_t bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
	if (bytes <= 0)
		return;

	buffer[bytes] = '\0';

	/* Parse request line */
	char method[16], path[256];
	if (sscanf(buffer, "%15s %255s", method, path) != 2) {
		send_http_response(client_fd, 400, "Bad Request",
				   "text/plain", "Invalid request\n");
		return;
	}

	/* Only accept GET requests */
	if (strcmp(method, "GET") != 0) {
		send_http_response(client_fd, 405, "Method Not Allowed",
				   "text/plain", "Only GET supported\n");
		return;
	}

	/* Route to handler */
	if (strcmp(path, "/health") == 0 || strcmp(path, "/healthz") == 0) {
		handle_health_request(client_fd);
	} else if (strcmp(path, "/metrics") == 0) {
		handle_metrics_request(client_fd);
	} else if (strcmp(path, "/ready") == 0 || strcmp(path, "/readyz") == 0) {
		handle_health_request(client_fd);  /* Same as health for now */
	} else {
		send_http_response(client_fd, 404, "Not Found",
				   "text/plain", "Not found\n");
	}
}

/* Health server thread */
static void *health_server_thread(void *arg)
{
	(void)arg;
	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);

	log_msg(LOG_INFO, "Health server started on port %d", health_port);

	while (!exit_req && health_thread_running) {
		struct pollfd pfd = {
			.fd = health_server_fd,
			.events = POLLIN
		};

		int ret = poll(&pfd, 1, 1000);  /* 1 second timeout */
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			log_msg(LOG_ERROR, "Poll error: %s", strerror(errno));
			break;
		}

		if (ret == 0)
			continue;  /* Timeout, check exit_req */

		int client_fd = accept(health_server_fd, (struct sockaddr *)&client_addr, &client_len);
		if (client_fd < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			log_msg(LOG_WARN, "Accept error: %s", strerror(errno));
			continue;
		}

		/* Set client socket to non-blocking with timeout */
		struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
		setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		handle_http_request(client_fd);
		close(client_fd);
	}

	log_msg(LOG_INFO, "Health server stopped");
	return NULL;
}

/* Start health HTTP server */
static int start_health_server(void)
{
	if (health_port <= 0)
		return 0;  /* Disabled */

	health_server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (health_server_fd < 0) {
		log_msg(LOG_ERROR, "Failed to create socket: %s", strerror(errno));
		return -1;
	}

	/* Allow address reuse */
	int opt = 1;
	setsockopt(health_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(health_port),
		.sin_addr.s_addr = INADDR_ANY
	};

	if (bind(health_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		log_msg(LOG_ERROR, "Failed to bind to port %d: %s", health_port, strerror(errno));
		close(health_server_fd);
		health_server_fd = -1;
		return -1;
	}

	if (listen(health_server_fd, 5) < 0) {
		log_msg(LOG_ERROR, "Failed to listen: %s", strerror(errno));
		close(health_server_fd);
		health_server_fd = -1;
		return -1;
	}

	health_thread_running = true;
	if (pthread_create(&health_thread, NULL, health_server_thread, NULL) != 0) {
		log_msg(LOG_ERROR, "Failed to create health thread: %s", strerror(errno));
		close(health_server_fd);
		health_server_fd = -1;
		health_thread_running = false;
		return -1;
	}

	return 0;
}

/* Stop health server with timeout */
static void stop_health_server(void)
{
	if (!health_thread_running)
		return;

	log_msg(LOG_DEBUG, "Stopping health server...");
	health_thread_running = false;

	if (health_server_fd >= 0) {
		shutdown(health_server_fd, SHUT_RDWR);
		close(health_server_fd);
		health_server_fd = -1;
	}

	/* Wait for thread with timeout */
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += CLEANUP_TIMEOUT_SEC;

	int ret = pthread_timedjoin_np(health_thread, NULL, &ts);
	if (ret == ETIMEDOUT) {
		log_msg(LOG_WARN, "Health thread join timed out, cancelling");
		pthread_cancel(health_thread);
		pthread_join(health_thread, NULL);
	}
}

/* Ring buffer callback for deadline miss events */
static int handle_deadline_event(void *ctx, void *data, size_t data_sz)
{
	(void)ctx;
	const struct deadline_event *event = data;

	if (data_sz < sizeof(*event)) {
		log_msg(LOG_ERROR, "Invalid event size: %zu", data_sz);
		return 0;
	}

	pthread_mutex_lock(&stats_lock);
	total_deadline_misses++;
	total_miss_duration_ns += event->deadline_miss_ns;
	pthread_mutex_unlock(&stats_lock);

	if (verbose) {
		log_msg(LOG_DEBUG, "DEADLINE MISS: cgroup=%llu miss=%.2fms timestamp=%llu",
			(unsigned long long)event->cgroup_id,
			ns_to_ms(event->deadline_miss_ns),
			(unsigned long long)event->timestamp);
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

	/* Update shared stats for metrics endpoint */
	pthread_mutex_lock(&stats_lock);
	last_local_dispatches = stats[0];
	last_global_dispatches = stats[1];
	pthread_mutex_unlock(&stats_lock);
}

static enum log_level parse_log_level(const char *level)
{
	if (strcasecmp(level, "debug") == 0) return LOG_DEBUG;
	if (strcasecmp(level, "info") == 0) return LOG_INFO;
	if (strcasecmp(level, "warn") == 0 || strcasecmp(level, "warning") == 0) return LOG_WARN;
	if (strcasecmp(level, "error") == 0) return LOG_ERROR;
	return LOG_INFO;  /* Default */
}

int main(int argc, char **argv)
{
	struct scx_slo *skel = NULL;
	struct bpf_link *link = NULL;
	struct ring_buffer *rb = NULL;
	int opt;
	__u64 ecode;
	int err = 0;

	libbpf_set_print(libbpf_print_fn);

	/* Set up signal handlers */
	struct sigaction sa = {
		.sa_handler = sigint_handler,
		.sa_flags = 0
	};
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	/* Handle --create-config first */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--create-config") == 0) {
			return create_example_config() == 0 ? 0 : 1;
		}
	}

restart:
	skel = SCX_OPS_OPEN(slo_ops, scx_slo);

	while ((opt = getopt(argc, argv, "vcp:jl:h")) != -1) {
		switch (opt) {
		case 'v':
			verbose = true;
			break;
		case 'c':
			reload_config = true;
			break;
		case 'p':
			health_port = atoi(optarg);
			break;
		case 'j':
			json_logging = true;
			break;
		case 'l':
			current_log_level = parse_log_level(optarg);
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}

	/* Reset getopt for potential restart */
	optind = 1;

	err = SCX_OPS_LOAD(skel, slo_ops, scx_slo, uei);
	if (err) {
		log_msg(LOG_ERROR, "Failed to load BPF program: %d", err);
		goto cleanup;
	}

	link = SCX_OPS_ATTACH(skel, slo_ops, scx_slo);
	if (!link) {
		log_msg(LOG_ERROR, "Failed to attach BPF program");
		err = -1;
		goto cleanup;
	}

	scheduler_attached = 1;
	log_msg(LOG_INFO, "BPF scheduler attached successfully");

	/* Set up ring buffer for deadline events */
	rb = ring_buffer__new(bpf_map__fd(skel->maps.deadline_events),
			      handle_deadline_event, NULL, NULL);
	if (!rb) {
		log_msg(LOG_ERROR, "Failed to create ring buffer");
		err = -1;
		goto cleanup;
	}

	/* Load SLO configuration if requested */
	if (reload_config) {
		int config_entries = load_slo_config(bpf_map__fd(skel->maps.slo_map));
		if (config_entries < 0) {
			log_msg(LOG_ERROR, "Failed to load configuration");
			err = -1;
			goto cleanup;
		}
		log_msg(LOG_INFO, "Loaded %d SLO configuration entries", config_entries);
	}

	/* Start health check server */
	if (start_health_server() < 0) {
		log_msg(LOG_WARN, "Failed to start health server (continuing without it)");
	}

	log_msg(LOG_INFO, "SLO scheduler started, press Ctrl-C to exit");

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		__u64 stats[2];

		/* Poll ring buffer for deadline events */
		err = ring_buffer__poll(rb, 100);  /* 100ms timeout */
		if (err < 0 && err != -EINTR) {
			log_msg(LOG_ERROR, "Error polling ring buffer: %d", err);
			break;
		}

		read_stats(skel, stats);

		/* Log stats at INFO level */
		pthread_mutex_lock(&stats_lock);
		__u64 misses = total_deadline_misses;
		__u64 miss_duration = total_miss_duration_ns;
		pthread_mutex_unlock(&stats_lock);

		if (json_logging) {
			printf("{\"timestamp\":\"%ld\",\"type\":\"stats\","
			       "\"local\":%llu,\"global\":%llu,"
			       "\"deadline_misses\":%llu,\"avg_miss_ms\":%.2f}\n",
			       time(NULL),
			       (unsigned long long)stats[0],
			       (unsigned long long)stats[1],
			       (unsigned long long)misses,
			       misses > 0 ? ns_to_ms(miss_duration / misses) : 0.0);
		} else {
			log_msg(LOG_INFO, "local=%llu global=%llu deadline_misses=%llu avg_miss=%.2fms",
				(unsigned long long)stats[0],
				(unsigned long long)stats[1],
				(unsigned long long)misses,
				misses > 0 ? ns_to_ms(miss_duration / misses) : 0.0);
		}

		sleep(1);
	}

	err = 0;  /* Clean exit */

cleanup:
	log_msg(LOG_INFO, "Initiating cleanup with %d second timeout", CLEANUP_TIMEOUT_SEC);
	scheduler_attached = 0;

	/* Stop health server first */
	stop_health_server();

	if (rb) {
		log_msg(LOG_DEBUG, "Freeing ring buffer");
		ring_buffer__free(rb);
		rb = NULL;
	}

	if (link) {
		log_msg(LOG_DEBUG, "Detaching BPF program");
		bpf_link__destroy(link);
		link = NULL;
	}

	if (skel) {
		ecode = UEI_REPORT(skel, uei);
		scx_slo__destroy(skel);
		skel = NULL;
		log_msg(LOG_INFO, "BPF scheduler detached successfully");

		if (UEI_ECODE_RESTART(ecode)) {
			log_msg(LOG_INFO, "Restarting scheduler");
			goto restart;
		}
	}

	/* Print final statistics */
	pthread_mutex_lock(&stats_lock);
	__u64 final_misses = total_deadline_misses;
	__u64 final_duration = total_miss_duration_ns;
	pthread_mutex_unlock(&stats_lock);

	if (final_misses > 0) {
		log_msg(LOG_INFO, "Final stats: %llu deadline misses, avg miss %.2fms",
			(unsigned long long)final_misses,
			ns_to_ms(final_duration / final_misses));
	} else {
		log_msg(LOG_INFO, "Final stats: No deadline misses detected");
	}

	log_msg(LOG_INFO, "Shutdown complete");
	return err;
}
