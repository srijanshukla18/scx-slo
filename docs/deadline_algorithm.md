# SLO Deadline Detection Algorithm

This document describes the corrected deadline miss detection algorithm implemented in scx-slo.

## Overview

The SLO scheduler enforces service-level latency budgets by tracking when tasks must complete to meet their deadlines. The deadline miss detection is critical for accurate SLO monitoring and enforcement.

## Algorithm Description

### 1. Deadline Calculation

When a task is enqueued, its deadline is calculated as:

```
deadline = enqueue_time + budget_ns
```

Where:
- `enqueue_time`: When the task was added to the runqueue (nanoseconds since epoch)
- `budget_ns`: The task's allocated latency budget in nanoseconds
- `deadline`: The absolute time by which the task must complete

### 2. Deadline Miss Detection

A deadline miss occurs when:

```
current_time > deadline
```

**Key Point**: We check if the current time exceeds the original deadline, NOT whether the task's runtime exceeds its budget.

### 3. Why This Matters

The corrected algorithm properly accounts for scheduling delays and preemption:

#### Incorrect Approach (Original Implementation)
```c
// WRONG: Only checks runtime vs budget
u64 runtime = now - task_start_time;
if (runtime > cfg->budget_ns) {
    // This misses deadline violations due to scheduling delays
}
```

#### Correct Approach (Fixed Implementation)
```c
// CORRECT: Checks current time vs original deadline
if (now > ctx->deadline) {
    u64 miss_duration = now - ctx->deadline;
    // Reports actual deadline miss regardless of cause
}
```

## Examples

### Example 1: Preemption-Induced Miss
- Task enqueued at: `t=1000ms`
- Budget: `100ms`
- Deadline: `t=1100ms`
- Task gets preempted until: `t=1150ms`
- Task runs for: `50ms` (completes at `t=1200ms`)

**Result**: Deadline miss of `100ms` (1200ms - 1100ms)
- The task only consumed 50ms of CPU but missed its deadline due to preemption
- The correct algorithm detects this; the incorrect one would not

### Example 2: CPU-Bound Miss
- Task enqueued at: `t=1000ms`
- Budget: `100ms`
- Deadline: `t=1100ms`
- Task starts immediately and runs for: `150ms` (completes at `t=1150ms`)

**Result**: Deadline miss of `50ms` (1150ms - 1100ms)
- Both algorithms would detect this, but for different reasons

### Example 3: No Miss Despite High CPU Usage
- Task enqueued at: `t=1000ms`
- Budget: `100ms`
- Deadline: `t=1100ms`
- Task starts immediately and runs for: `90ms` (completes at `t=1090ms`)

**Result**: No deadline miss
- Task completed before deadline despite using 90% of its budget

## Implementation Details

### Task Context Structure
```c
struct slo_task_ctx {
    u64 deadline;       /* Absolute deadline timestamp */
    u64 start_time;     /* When task started running */
    u64 budget_ns;      /* Task's allocated budget */
    u32 valid;          /* Context validity flag */
};
```

### Key Functions

#### Deadline Calculation (in `enqueue`)
```c
u64 now = bpf_ktime_get_ns();
u64 deadline = now + budget_ns;
ctx->deadline = deadline;
```

#### Deadline Miss Detection (in `stopping`)
```c
u64 now = bpf_ktime_get_ns();
if (now > ctx->deadline) {
    u64 miss_duration = now - ctx->deadline;
    // Report deadline miss event
}
```

## Benefits of the Correct Algorithm

1. **Accurate SLO Monitoring**: Detects all deadline violations, regardless of cause
2. **Preemption Awareness**: Accounts for scheduling delays beyond task control
3. **Resource Contention Detection**: Identifies when system load affects latency
4. **Fair Assessment**: Doesn't penalize tasks for system-level delays

## Rate Limiting

To prevent spam attacks, deadline miss events are rate-limited to 1000 events per second per CPU.

## Validation Bounds

- **Minimum Budget**: 1ms (prevents DoS attacks with zero/tiny budgets)
- **Maximum Budget**: 10s (prevents overflow attacks)
- **Default Budget**: 100ms (reasonable default for most workloads)

## Security Considerations

The deadline algorithm includes several security measures:

1. **Input Validation**: All budget values are bounded and validated
2. **Rate Limiting**: Prevents deadline miss event flooding
3. **Overflow Protection**: Uses safe arithmetic for deadline calculations
4. **Context Cleanup**: Properly manages task context lifecycle

This corrected algorithm provides accurate, secure, and reliable deadline miss detection for SLO enforcement.