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

/* SLO configuration per cgroup */
struct slo_cfg {
	u64 budget_ns;      /* Latency budget in nanoseconds */
	u32 importance;     /* Relative importance (1-100) */
	u32 flags;          /* Configuration flags */
};

/* Map: cgroup_id -> SLO configuration */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(key_size, sizeof(u64));
	__uint(value_size, sizeof(struct slo_cfg));
	__uint(max_entries, 10000);
} slo_map SEC(".maps");

/* Ring buffer for deadline miss events */
struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 1 << 20);  /* 1MB */
} deadline_events SEC(".maps");

struct deadline_event {
	u64 cgroup_id;
	u64 deadline_miss_ns;
	u64 timestamp;
};

/* Default SLO budget for tasks without configuration (100ms) */
#define DEFAULT_BUDGET_NS (100 * NSEC_PER_MSEC)

UEI_DEFINE(uei);

/*
 * We use a custom DSQ for deadline-based scheduling
 */
#define SHARED_DSQ 0

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, 2);			/* [local, global] */
} stats SEC(".maps");

static void stat_inc(u32 idx)
{
	u64 *cnt_p = bpf_map_lookup_elem(&stats, &idx);
	if (cnt_p)
		(*cnt_p)++;
}

s32 BPF_STRUCT_OPS(simple_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
	bool is_idle = false;
	s32 cpu;

	cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
	if (is_idle) {
		stat_inc(0);	/* count local queueing */
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0);
	}

	return cpu;
}

void BPF_STRUCT_OPS(simple_enqueue, struct task_struct *p, u64 enq_flags)
{
	stat_inc(1);	/* count global queueing */

	/* Get cgroup ID and look up SLO configuration */
	u64 cg_id = bpf_get_current_cgroup_id();
	struct slo_cfg *cfg = bpf_map_lookup_elem(&slo_map, &cg_id);
	
	u64 budget_ns = DEFAULT_BUDGET_NS;
	if (cfg) {
		budget_ns = cfg->budget_ns;
	}
	
	/* Calculate virtual deadline = current time + budget */
	u64 now = bpf_ktime_get_ns();
	u64 deadline = now + budget_ns;
	
	/* Use vtime for deadline-based scheduling */
	p->scx.dsq_vtime = deadline;
	
	/* Insert task with deadline as vtime for earliest-deadline-first */
	scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, SCX_SLICE_DFL, deadline, enq_flags);
}

void BPF_STRUCT_OPS(simple_dispatch, s32 cpu, struct task_struct *prev)
{
	scx_bpf_dsq_move_to_local(SHARED_DSQ);
}

void BPF_STRUCT_OPS(simple_running, struct task_struct *p)
{
	/* Track when task started running for deadline miss detection */
	p->scx.dsq_vtime = bpf_ktime_get_ns();
}

void BPF_STRUCT_OPS(simple_stopping, struct task_struct *p, bool runnable)
{
	/* Check for deadline miss */
	u64 now = bpf_ktime_get_ns();
	u64 cg_id = bpf_get_current_cgroup_id();
	struct slo_cfg *cfg = bpf_map_lookup_elem(&slo_map, &cg_id);
	
	if (cfg && p->scx.dsq_vtime > 0) {
		u64 runtime = now - p->scx.dsq_vtime;
		if (runtime > cfg->budget_ns) {
			/* Report deadline miss */
			struct deadline_event *event;
			event = bpf_ringbuf_reserve(&deadline_events, sizeof(*event), 0);
			if (event) {
				event->cgroup_id = cg_id;
				event->deadline_miss_ns = runtime - cfg->budget_ns;
				event->timestamp = now;
				bpf_ringbuf_submit(event, 0);
			}
		}
	}
}

void BPF_STRUCT_OPS(simple_enable, struct task_struct *p)
{
	/* Initialize with current time for new tasks */
	p->scx.dsq_vtime = bpf_ktime_get_ns();
}

s32 BPF_STRUCT_OPS_SLEEPABLE(simple_init)
{
	return scx_bpf_create_dsq(SHARED_DSQ, -1);
}

void BPF_STRUCT_OPS(simple_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(slo_ops,
	       .select_cpu		= (void *)simple_select_cpu,
	       .enqueue			= (void *)simple_enqueue,
	       .dispatch		= (void *)simple_dispatch,
	       .running			= (void *)simple_running,
	       .stopping		= (void *)simple_stopping,
	       .enable			= (void *)simple_enable,
	       .init			= (void *)simple_init,
	       .exit			= (void *)simple_exit,
	       .name			= "scx_slo");
