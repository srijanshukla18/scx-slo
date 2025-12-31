/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SLO configuration interface for scx-slo scheduler
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <bpf/bpf.h>
#include "scx_slo.h"

/* Linux-specific includes for name_to_handle_at */
#ifdef __linux__
#include <linux/limits.h>
#endif

/* Fallback definitions for portability */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef MAX_HANDLE_SZ
#define MAX_HANDLE_SZ 128
#endif

/* file_handle structure for name_to_handle_at() */
#ifndef __linux__
struct file_handle {
	unsigned int handle_bytes;
	int handle_type;
	unsigned char f_handle[0];
};
#endif

#define CONFIG_FILE_PATH "/etc/scx-slo/config"
#define MAX_LINE_LENGTH 256
#define MAX_CGROUP_PATH 512
#define CGROUP_FS_ROOT "/sys/fs/cgroup"

struct slo_config_entry {
	char cgroup_path[MAX_CGROUP_PATH];
	__u64 budget_ms;
	__u32 importance;
};

/*
 * Validate cgroup path for security - prevent path traversal attacks.
 * Returns 0 on success, -1 on failure.
 */
static int validate_cgroup_path(const char *path)
{
	if (!path || strlen(path) == 0) {
		fprintf(stderr, "Error: Empty cgroup path\n");
		return -1;
	}

	/* Path must start with / (absolute within cgroup hierarchy) */
	if (path[0] != '/') {
		fprintf(stderr, "Error: Cgroup path must be absolute (start with /): %s\n", path);
		return -1;
	}

	/* Check for path traversal attempts */
	if (strstr(path, "..") != NULL) {
		fprintf(stderr, "Error: Path traversal detected in cgroup path: %s\n", path);
		return -1;
	}

	/* Check path length */
	if (strlen(path) >= MAX_CGROUP_PATH - strlen(CGROUP_FS_ROOT) - 1) {
		fprintf(stderr, "Error: Cgroup path too long: %s\n", path);
		return -1;
	}

	/* Check for null bytes in path (shouldn't happen with sscanf but be safe) */
	for (size_t i = 0; i < strlen(path); i++) {
		if (path[i] == '\0')
			break;
		/* Allow alphanumeric, /, -, _, . */
		char c = path[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '/' || c == '-' ||
		      c == '_' || c == '.')) {
			fprintf(stderr, "Error: Invalid character '%c' in cgroup path: %s\n", c, path);
			return -1;
		}
	}

	return 0;
}

/*
 * Convert cgroup path to cgroup ID using name_to_handle_at().
 * This matches what bpf_get_current_cgroup_id() returns in the kernel.
 *
 * The kernel's cgroup ID is derived from the cgroup's kernfs node ID,
 * which can be obtained via name_to_handle_at() on cgroup v2.
 */
static __u64 cgroup_path_to_id(const char *path)
{
	char full_path[MAX_CGROUP_PATH];
	char resolved_path[PATH_MAX];
	int fd = -1;
	__u64 cgroup_id = 0;
	int mount_id;

	/* Build full path */
	int ret = snprintf(full_path, sizeof(full_path), "%s%s", CGROUP_FS_ROOT, path);
	if (ret < 0 || (size_t)ret >= sizeof(full_path)) {
		fprintf(stderr, "Error: Path too long: %s\n", path);
		return 0;
	}

	/* Resolve to canonical path and verify it stays under cgroup root */
	if (realpath(full_path, resolved_path) == NULL) {
		fprintf(stderr, "Error: Cannot resolve cgroup path %s: %s\n",
			full_path, strerror(errno));
		return 0;
	}

	/* Security check: ensure resolved path is under cgroup root */
	if (strncmp(resolved_path, CGROUP_FS_ROOT, strlen(CGROUP_FS_ROOT)) != 0) {
		fprintf(stderr, "Error: Resolved path escapes cgroup root: %s -> %s\n",
			full_path, resolved_path);
		return 0;
	}

	/* Open the cgroup directory */
	fd = open(resolved_path, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
	if (fd < 0) {
		fprintf(stderr, "Error: Cannot open cgroup %s: %s\n",
			resolved_path, strerror(errno));
		return 0;
	}

	/*
	 * Use name_to_handle_at to get the file handle, which contains
	 * the cgroup ID that matches bpf_get_current_cgroup_id().
	 *
	 * The handle contains an opaque identifier that for cgroup v2
	 * filesystems is the cgroup ID (kernfs node id).
	 */
	struct {
		struct file_handle handle;
		unsigned char buf[MAX_HANDLE_SZ];
	} fh;

	fh.handle.handle_bytes = MAX_HANDLE_SZ;

	if (name_to_handle_at(fd, "", &fh.handle, &mount_id, AT_EMPTY_PATH) < 0) {
		/* Fallback to inode-based ID if name_to_handle_at fails */
		struct stat st;
		if (fstat(fd, &st) == 0) {
			/*
			 * On older kernels or non-cgroup2 filesystems, fall back
			 * to using the inode number. This may not match
			 * bpf_get_current_cgroup_id() but is better than nothing.
			 */
			cgroup_id = st.st_ino;
			fprintf(stderr, "Warning: Using inode fallback for %s (may not match kernel ID)\n",
				path);
		}
		close(fd);
		return cgroup_id;
	}

	close(fd);

	/*
	 * Extract cgroup ID from file handle.
	 * For cgroup2, the handle contains the 64-bit cgroup ID.
	 */
	if (fh.handle.handle_bytes >= sizeof(__u64)) {
		memcpy(&cgroup_id, fh.handle.f_handle, sizeof(__u64));
	} else if (fh.handle.handle_bytes >= sizeof(__u32)) {
		/* Some systems may use 32-bit handles */
		__u32 id32;
		memcpy(&id32, fh.handle.f_handle, sizeof(__u32));
		cgroup_id = id32;
	}

	return cgroup_id;
}

/* Validate SLO configuration entry */
static int validate_config_entry(const struct slo_config_entry *entry)
{
	/* Security: validate cgroup path first to prevent path traversal */
	if (validate_cgroup_path(entry->cgroup_path) != 0) {
		return -1;
	}

	if (entry->budget_ms < MIN_BUDGET_NS / 1000000 ||
	    entry->budget_ms > MAX_BUDGET_NS / 1000000) {
		fprintf(stderr, "Invalid budget %llu ms (must be %llu-%llu ms)\n",
			(unsigned long long)entry->budget_ms,
			(unsigned long long)(MIN_BUDGET_NS / 1000000),
			(unsigned long long)(MAX_BUDGET_NS / 1000000));
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