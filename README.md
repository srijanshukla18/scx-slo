# scx-slo

An eBPF-based Linux CPU scheduler that enforces service-level latency budgets directly in the kernel. Protect your latency-sensitive Kubernetes workloads from noisy neighbors on saturated nodes.

## Why

When nodes hit 100% CPU, standard Linux scheduling (CFS) treats every process "fairly," which causes p99 latency spikes for critical services. `scx-slo` brings **business intent** into the kernel, ensuring that a high-priority payment API gets its CPU cycles before a background batch worker, regardless of their relative CPU usage.

## How It Works: The "Brain & Brawn" Architecture

```
+-------------------+                    +-----------------------+
|  Go K8s Watcher   |                    |   C Userspace Agent   |
| (Node-Side Brain) |                    |   (BPF Loader/Metrics)|
| - Watches Pods    |                    | - Loads BPF prog      |
| - Reads Annotations                    | - Exposes /metrics    |
+---------|---------+                    +-----------|-----------+
          |                                          |
          | updates pinned maps                      | logic
          v                                          v
+------------------------------------------------------------+
|                        eBPF Kernel Maps                    |
|  [cgroup_id -> {budget_ns, importance}] | [task_ctx_map]   |
+------------------------------------------------------------+
                             |
                  Earliest Deadline First (EDF)
                             |
                             v
                 Kernel run-queue decisions
```

1.  **Earliest Deadline First (EDF)**: Tasks are prioritized based on a virtual deadline: `now + (budget_ns * (101 - importance) / 100)`.
2.  **K8s Native Integration**: A Go-based sidecar (`watcher`) tracks Pods on the node and automatically translates `scx-slo/` annotations into kernel-side configs.
3.  **Cgroup Resolution**: The watcher dynamically resolves Pod UIDs to 64-bit Kernel Cgroup IDs using `name_to_handle_at()`.

## Deployment

Deploy the scheduler and watcher to all nodes (Linux 6.12+ required):

```bash
kubectl apply -f scx-slo-daemonset.yaml
```

## Usage

Simply annotate your Pods to opt-in to SLO scheduling:

```yaml
metadata:
  annotations:
    scx-slo/budget-ms: "20"    # Target p99 latency (ms)
    scx-slo/importance: "95"   # Relative priority (1-100)
```

## Security & Resilience

-   **Least Privilege**: Runs with specific capabilities (`CAP_BPF`, `CAP_SYS_ADMIN`, `CAP_PERFMON`) instead of `privileged: true`.
-   **Safe Arithmetic**: Uses saturating arithmetic to prevent integer overflows in deadline calculations.
-   **Automatic Fallback**: If the scheduler crashes or is detached, the kernel gracefully reverts to the default CFS scheduler immediately.
-   **Rate Limiting**: Deadline miss events are rate-limited per-CPU to prevent BPF-to-userspace flooding.

## Monitoring

Metrics are exposed on port `8080`:

```bash
curl localhost:8080/metrics | grep scx_slo
```

-   `scx_slo_deadline_misses`: Total count of tasks exceeding their budget.
-   `scx_slo_dispatch_local`: Total scheduling decisions made.

## Development & Testing

### Build
The project uses a multi-stage Dockerfile that compiles both the C agent and the Go watcher.
```bash
docker build -t scx-slo:latest .
```

### Run Tests
Verification suite for deadline logic and security boundaries:
```bash
make test
```

## License
GPL-2.0 (Required for `sched_ext` BPF programs)
