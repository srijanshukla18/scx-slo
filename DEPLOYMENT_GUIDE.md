# scx-slo Deployment Guide

## What We Built

We've created a simplified SLO-aware CPU scheduler based on the sched_ext framework:

1. **scx_slo.bpf.c** - The eBPF scheduler that implements:
   - Deadline-based scheduling (earliest deadline first)
   - Per-cgroup SLO configuration via BPF maps
   - Deadline miss detection and reporting
   - Default 100ms budget for unconfigured workloads

2. **Dockerfile** - Multi-stage build that:
   - Compiles the BPF program in Ubuntu 24.04
   - Includes scx_loader from the sched-ext project
   - Creates a minimal deployment image

3. **Kubernetes manifests**:
   - DaemonSet that deploys the scheduler to all nodes
   - Demo workloads (frontend + batch stressor)
   - ConfigMap for future SLO configuration

## Quick Start

### 1. Build the Container Image

```bash
# Clone this repo
git clone <your-repo>
cd scx-slo

# Build the image
./build.sh

# Or manually:
docker build -t ghcr.io/yourorg/scx-slo-loader:v0.1.0 .
docker push ghcr.io/yourorg/scx-slo-loader:v0.1.0
```

### 2. Deploy to Kubernetes

```bash
# Update the image in the DaemonSet if needed
kubectl apply -f scx-slo-daemonset.yaml

# Verify deployment
kubectl get pods -n kube-system -l app=scx-slo
```

### 3. Test with Demo Workloads

```bash
# Deploy test workloads
kubectl apply -f demo-workloads.yaml

# Get frontend service IP
kubectl get svc frontend

# Run load test
wrk -t2 -c64 -d30s http://<frontend-ip>/delay/0.2
```

## How It Works

1. The DaemonSet runs on each node with privileged access
2. It loads the scx_slo BPF program into the kernel
3. The scheduler assigns virtual deadlines to tasks based on their cgroup
4. Tasks are scheduled in earliest-deadline-first order
5. Deadline misses are tracked for monitoring (future enhancement)

## Current Limitations

This is a proof-of-concept that demonstrates the core concept. Production features not yet implemented:

- [ ] Dynamic SLO configuration from pod annotations
- [ ] Userspace agent to update BPF maps
- [ ] Metrics export for Prometheus
- [ ] Early HTTP 429/503 responses on overload
- [ ] Graceful degradation and backpressure

## Requirements

- Kubernetes nodes with Linux kernel 6.12+
- Bottlerocket or Ubuntu nodes with sched_ext support
- Privileged container execution enabled

## Next Steps

1. **Add SLO Agent**: Create a sidecar that reads pod annotations and updates the BPF map
2. **Metrics Export**: Add Prometheus metrics for deadline misses
3. **Production Hardening**: Add health checks, resource limits, and observability
4. **Helm Chart**: Package for easier deployment and configuration

## Troubleshooting

Check if scheduler is loaded:
```bash
kubectl exec -n kube-system <scx-slo-pod> -- cat /sys/kernel/sched_ext/ops
```

View logs:
```bash
kubectl logs -n kube-system -l app=scx-slo
```

## Architecture Notes

The current implementation uses a single global deadline-ordered queue. This works well for uniform CPU topologies but may need enhancement for NUMA systems. The scheduler automatically falls back to CFS on any BPF verification failure or runtime error.