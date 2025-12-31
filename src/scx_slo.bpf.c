/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SLO-aware scheduler (scx-slo)
 *
 * This scheduler enforces service-level latency budgets at the kernel level.
 * It uses deadline-based scheduling where tasks are prioritized based on their
 * virtual deadlines computed from SLO budgets.
 *
 * Features:
 * - Per-cgroup SLO configuration (latency budget in nanoseconds)
 * - Virtual deadline scheduling (deadline = last_runtime + budget)
 * - Deadline miss detection and reporting
 * - Graceful fallback for tasks without SLO configuration
 *
 * Based on scx_simple scheduler framework.
 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

/* Maximum value for u64 - used for overflow protection */
#ifndef U64_MAX
#define U64_MAX ((u64)~0ULL)
#endif

/* SLO configuration per cgroup */
struct slo_cfg {
  u64 budget_ns;  /* Latency budget in nanoseconds */
  u32 importance; /* Relative importance (1-100) */
  u32 flags;      /* Configuration flags */
};

/* Per-task scheduling context - replaces dsq_vtime abuse */
struct slo_task_ctx {
  u64 deadline;   /* When this task must complete by */
  u64 start_time; /* When task started running (for miss detection) */
  u64 budget_ns;  /* Task's allocated budget */
  u32 valid;      /* Whether this context is initialized */
};

/* Map: cgroup_id -> SLO configuration */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(key_size, sizeof(u64));
  __uint(value_size, sizeof(struct slo_cfg));
  __uint(max_entries, MAX_CGROUPS);
  __uint(pinning, LIBBPF_PIN_BY_NAME);
} slo_map SEC(".maps");

/* Map: task PID -> per-task scheduling context */
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(key_size, sizeof(u32));
  __uint(value_size, sizeof(struct slo_task_ctx));
  __uint(max_entries, MAX_TASKS);
  __uint(pinning, LIBBPF_PIN_BY_NAME);
} task_ctx_map SEC(".maps");

/* Rate limiting state for ring buffer events */
struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(key_size, sizeof(u32));
  __uint(value_size, sizeof(u64));
  __uint(max_entries, RATE_LIMIT_MAP_ENTRIES);
} rate_limit_state SEC(".maps");

/* Ring buffer for deadline miss events */
struct {
  __uint(type, BPF_MAP_TYPE_RINGBUF);
  __uint(max_entries, RINGBUF_SIZE);
} deadline_events SEC(".maps");

struct deadline_event {
  u64 cgroup_id;
  u64 deadline_miss_ns;
  u64 timestamp;
};

/* SLO budget constants with validation bounds */
#define DEFAULT_BUDGET_NS (100 * NSEC_PER_MSEC) /* 100ms default */
#define MIN_BUDGET_NS (1 * NSEC_PER_MSEC)       /* 1ms minimum */
#define MAX_BUDGET_NS (10 * NSEC_PER_SEC)       /* 10s maximum */

/* Importance value bounds */
#define MIN_IMPORTANCE 1
#define MAX_IMPORTANCE 100

/* Rate limiting for ring buffer events */
#define MAX_EVENTS_PER_SEC 1000
#define RATE_LIMIT_WINDOW_NS (1 * NSEC_PER_SEC)

/* Security constants */
#define SLO_MAP_UPDATE_CAP_SYS_ADMIN 1
#define SLO_MAP_UPDATE_CAP_BPF 2

UEI_DEFINE(uei);

/*
 * Scheduling constants and DSQ definitions
 */
#define SHARED_DSQ 0
#define LOCAL_DSQ_ID 1

/* Map sizing constants */
#define MAX_CGROUPS 10000
#define MAX_TASKS 100000
#define RINGBUF_SIZE (1 << 20)   /* 1MB */
#define STATS_MAP_ENTRIES 2      /* [local, global] */
#define RATE_LIMIT_MAP_ENTRIES 2 /* [event_count, window_start] */

struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(key_size, sizeof(u32));
  __uint(value_size, sizeof(u64));
  __uint(max_entries, STATS_MAP_ENTRIES);
} stats SEC(".maps");

static void stat_inc(u32 idx) {
  u64 *cnt_p = bpf_map_lookup_elem(&stats, &idx);
  if (cnt_p)
    (*cnt_p)++;
}

/* Validate SLO configuration to prevent DoS attacks */
static inline int validate_slo_cfg(struct slo_cfg *cfg) {
  if (!cfg)
    return -1;

  /* Validate budget bounds - prevent 0 (infinite priority) and UINT64_MAX
   * (overflow) */
  if (cfg->budget_ns == 0 || cfg->budget_ns < MIN_BUDGET_NS ||
      cfg->budget_ns > MAX_BUDGET_NS)
    return -1;

  /* Validate importance bounds */
  if (cfg->importance < MIN_IMPORTANCE || cfg->importance > MAX_IMPORTANCE)
    return -1;

  return 0;
}

/* Validate and get safe budget value */
static inline u64 get_safe_budget(u64 cgroup_id) {
  struct slo_cfg *cfg = bpf_map_lookup_elem(&slo_map, &cgroup_id);
  if (!cfg)
    return DEFAULT_BUDGET_NS;

  /* Use validation function to check for attacks */
  if (validate_slo_cfg(cfg) != 0)
    return DEFAULT_BUDGET_NS;

  return cfg->budget_ns;
}

/* Rate limit ring buffer events to prevent spam attacks */
static inline bool is_rate_limited(void) {
  u64 now = bpf_ktime_get_ns();
  u32 count_idx = 0, window_idx = 1;

  u64 *event_count = bpf_map_lookup_elem(&rate_limit_state, &count_idx);
  u64 *window_start = bpf_map_lookup_elem(&rate_limit_state, &window_idx);

  if (!event_count || !window_start)
    return true; /* Fail closed on map errors */

  /* Reset window if needed */
  if (now - *window_start > RATE_LIMIT_WINDOW_NS) {
    *window_start = now;
    *event_count = 0;
  }

  /* Check if we're over the limit */
  if (*event_count >= MAX_EVENTS_PER_SEC)
    return true;

  /* Increment counter */
  (*event_count)++;
  return false;
}

/* Get or create task context */
static struct slo_task_ctx *get_task_ctx(u32 pid) {
  struct slo_task_ctx *ctx = bpf_map_lookup_elem(&task_ctx_map, &pid);
  if (ctx)
    return ctx;

  /* Create new context */
  struct slo_task_ctx new_ctx = {
      .deadline = 0, .start_time = 0, .budget_ns = 0, .valid = 0};

  if (bpf_map_update_elem(&task_ctx_map, &pid, &new_ctx, BPF_ANY) == 0)
    return bpf_map_lookup_elem(&task_ctx_map, &pid);

  return NULL;
}

s32 BPF_STRUCT_OPS(simple_select_cpu, struct task_struct *p, s32 prev_cpu,
                   u64 wake_flags) {
  bool is_idle = false;
  s32 cpu;

  cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
  if (is_idle) {
    stat_inc(0); /* count local queueing */
    scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0);
  }

  return cpu;
}

void BPF_STRUCT_OPS(simple_enqueue, struct task_struct *p, u64 enq_flags) {
  stat_inc(1); /* count global queueing */

  u32 pid = p->pid;
  u64 cg_id = bpf_get_current_cgroup_id();
  u64 now = bpf_ktime_get_ns();

  /* Get validated budget for this cgroup */
  u64 budget_ns = get_safe_budget(cg_id);

  /* Get or create task context */
  struct slo_task_ctx *ctx = get_task_ctx(pid);
  if (!ctx) {
    /* Fallback: use default scheduling without context */
    scx_bpf_dsq_insert(p, SHARED_DSQ, SCX_SLICE_DFL, enq_flags);
    return;
  }

  /*
   * Calculate deadline with weighted importance and overflow protection.
   * Higher importance (1-100) results in a shorter virtual budget,
   * giving the task an earlier deadline in the EDF queue.
   *
   * Formula: effective_budget = budget_ns * (101 - importance) / 100
   */
  struct slo_cfg *cfg = bpf_map_lookup_elem(&slo_map, &cg_id);
  u32 importance = cfg ? cfg->importance : 50;
  if (importance < 1)
    importance = 1;
  if (importance > 100)
    importance = 100;

  u64 scaling_factor = 101 - importance;
  u64 effective_budget = (budget_ns * scaling_factor) / 100;

  u64 deadline;
  if (effective_budget > U64_MAX - now) {
    /* Overflow would occur, saturate at maximum */
    deadline = U64_MAX;
  } else {
    deadline = now + effective_budget;
  }

  /* Store context properly instead of abusing dsq_vtime */
  ctx->deadline = deadline;
  ctx->budget_ns = budget_ns;
  ctx->start_time = 0; /* Will be set when task starts running */
  ctx->valid = 1;

  /* Insert task with deadline as vtime for earliest-deadline-first */
  scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, SCX_SLICE_DFL, deadline, enq_flags);
}

void BPF_STRUCT_OPS(simple_dispatch, s32 cpu, struct task_struct *prev) {
  scx_bpf_dsq_move_to_local(SHARED_DSQ);
}

void BPF_STRUCT_OPS(simple_running, struct task_struct *p) {
  u32 pid = p->pid;
  struct slo_task_ctx *ctx = get_task_ctx(pid);

  if (ctx && ctx->valid) {
    /* Record when task actually started running */
    ctx->start_time = bpf_ktime_get_ns();
  }
}

void BPF_STRUCT_OPS(simple_stopping, struct task_struct *p, bool runnable) {
  u32 pid = p->pid;
  u64 now = bpf_ktime_get_ns();
  struct slo_task_ctx *ctx = bpf_map_lookup_elem(&task_ctx_map, &pid);

  if (!ctx || !ctx->valid)
    return;

  /* CORRECT deadline miss detection: check if current time > original deadline
   */
  if (now > ctx->deadline) {
    u64 cg_id = bpf_get_current_cgroup_id();
    u64 miss_duration = now - ctx->deadline;

    /* Report deadline miss with rate limiting to prevent spam */
    if (!is_rate_limited()) {
      struct deadline_event *event;
      event = bpf_ringbuf_reserve(&deadline_events, sizeof(*event), 0);
      if (event) {
        event->cgroup_id = cg_id;
        event->deadline_miss_ns = miss_duration;
        event->timestamp = now;
        bpf_ringbuf_submit(event, 0);
      }
    }
  }

  /* Clean up task context when task stops */
  if (!runnable) {
    bpf_map_delete_elem(&task_ctx_map, &pid);
  }
}

void BPF_STRUCT_OPS(simple_enable, struct task_struct *p) {
  /* Initialize task context for new tasks */
  u32 pid = p->pid;
  struct slo_task_ctx *ctx = get_task_ctx(pid);

  /* Context will be properly initialized in enqueue */
}

s32 BPF_STRUCT_OPS_SLEEPABLE(simple_init) {
  return scx_bpf_create_dsq(SHARED_DSQ, -1);
}

void BPF_STRUCT_OPS(simple_exit, struct scx_exit_info *ei) {
  UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(slo_ops, .select_cpu = (void *)simple_select_cpu,
               .enqueue = (void *)simple_enqueue,
               .dispatch = (void *)simple_dispatch,
               .running = (void *)simple_running,
               .stopping = (void *)simple_stopping,
               .enable = (void *)simple_enable, .init = (void *)simple_init,
               .exit = (void *)simple_exit, .name = "scx_slo");
