/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SLO configuration interface for scx-slo scheduler
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <bpf/bpf.h>
#include "scx_slo.h"

#define CONFIG_FILE_PATH "/etc/scx-slo/config"
#define MAX_LINE_LENGTH 256
#define MAX_CGROUP_PATH 512

struct slo_config_entry {
	char cgroup_path[MAX_CGROUP_PATH];
	__u64 budget_ms;
	__u32 importance;
};

/* Convert cgroup path to cgroup ID (simplified implementation) */
static __u64 cgroup_path_to_id(const char *path)
{
	/* This is a simplified implementation - in production this would
	 * need proper cgroup v1/v2 detection and path resolution */
	char cgroup_path[MAX_CGROUP_PATH];
	int fd;
	__u64 cgroup_id = 0;
	
	snprintf(cgroup_path, sizeof(cgroup_path), "/sys/fs/cgroup%s", path);
	
	fd = open(cgroup_path, O_RDONLY);
	if (fd >= 0) {
		/* Use inode number as a simple cgroup ID */
		struct stat st;
		if (fstat(fd, &st) == 0) {
			cgroup_id = st.st_ino;
		}
		close(fd);
	}
	
	return cgroup_id;
}

/* Validate SLO configuration entry */
static int validate_config_entry(const struct slo_config_entry *entry)
{
	if (strlen(entry->cgroup_path) == 0) {
		fprintf(stderr, "Empty cgroup path\n");
		return -1;
	}
	
	if (entry->budget_ms < MIN_BUDGET_NS / 1000000 || 
	    entry->budget_ms > MAX_BUDGET_NS / 1000000) {
		fprintf(stderr, "Invalid budget %llu ms (must be %llu-%llu ms)\n",
			entry->budget_ms, 
			MIN_BUDGET_NS / 1000000,
			MAX_BUDGET_NS / 1000000);
		return -1;
	}
	
	if (entry->importance < MIN_IMPORTANCE || entry->importance > MAX_IMPORTANCE) {
		fprintf(stderr, "Invalid importance %u (must be %u-%u)\n",
			entry->importance, MIN_IMPORTANCE, MAX_IMPORTANCE);
		return -1;
	}
	
	return 0;
}

/* Parse configuration file and update BPF maps */
int load_slo_config(int slo_map_fd)
{
	FILE *config_file;
	char line[MAX_LINE_LENGTH];
	struct slo_config_entry entry;
	struct slo_cfg cfg;
	int line_num = 0;
	int entries_loaded = 0;
	
	config_file = fopen(CONFIG_FILE_PATH, "r");
	if (!config_file) {
		if (errno == ENOENT) {
			printf("No config file found at %s, using defaults\n", CONFIG_FILE_PATH);
			return 0;
		}
		fprintf(stderr, "Failed to open config file %s: %s\n", 
			CONFIG_FILE_PATH, strerror(errno));
		return -1;
	}
	
	printf("Loading SLO configuration from %s\n", CONFIG_FILE_PATH);
	
	while (fgets(line, sizeof(line), config_file)) {
		line_num++;
		
		/* Skip comments and empty lines */
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
			continue;
		
		/* Parse line: cgroup_path budget_ms importance */
		if (sscanf(line, "%511s %llu %u", 
			   entry.cgroup_path, &entry.budget_ms, &entry.importance) != 3) {
			fprintf(stderr, "Invalid config line %d: %s", line_num, line);
			continue;
		}
		
		/* Validate entry */
		if (validate_config_entry(&entry) != 0) {
			fprintf(stderr, "Invalid config at line %d\n", line_num);
			continue;
		}
		
		/* Convert to BPF format */
		__u64 cgroup_id = cgroup_path_to_id(entry.cgroup_path);
		if (cgroup_id == 0) {
			fprintf(stderr, "Failed to resolve cgroup %s at line %d\n", 
				entry.cgroup_path, line_num);
			continue;
		}
		
		cfg.budget_ns = entry.budget_ms * 1000000ULL;  /* ms to ns */
		cfg.importance = entry.importance;
		cfg.flags = 0;
		
		/* Update BPF map */
		if (bpf_map_update_elem(slo_map_fd, &cgroup_id, &cfg, BPF_ANY) != 0) {
			fprintf(stderr, "Failed to update BPF map for cgroup %s: %s\n",
				entry.cgroup_path, strerror(errno));
			continue;
		}
		
		printf("Loaded SLO config: %s -> %llu ms, importance %u\n",
		       entry.cgroup_path, entry.budget_ms, entry.importance);
		entries_loaded++;
	}
	
	fclose(config_file);
	printf("Loaded %d SLO configuration entries\n", entries_loaded);
	return entries_loaded;
}

/* Create example configuration file */
int create_example_config(void)
{
	FILE *config_file;
	const char *example_config = 
		"# SLO Scheduler Configuration\n"
		"# Format: cgroup_path budget_ms importance\n"
		"# \n"
		"# Examples:\n"
		"/kubepods/critical/payment-api 50 90\n"
		"/kubepods/standard/user-service 100 70\n"
		"/kubepods/batch/analytics 500 20\n"
		"# \n"
		"# Budget: 1-10000 ms (latency budget)\n"
		"# Importance: 1-100 (relative priority)\n";
	
	/* Create directory if it doesn't exist */
	if (mkdir("/etc/scx-slo", 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "Failed to create config directory: %s\n", strerror(errno));
		return -1;
	}
	
	config_file = fopen(CONFIG_FILE_PATH, "w");
	if (!config_file) {
		fprintf(stderr, "Failed to create example config: %s\n", strerror(errno));
		return -1;
	}
	
	fprintf(config_file, "%s", example_config);
	fclose(config_file);
	
	printf("Created example configuration at %s\n", CONFIG_FILE_PATH);
	return 0;
}