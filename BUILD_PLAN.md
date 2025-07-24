# Build Plan for scx-slo eBPF Scheduler

## Overview
This document outlines the build plan for implementing the scx-slo (SLO-aware CPU scheduler) using eBPF and the sched_ext interface.

## 1. Project Structure Setup

```
scx-slo/
├── src/
│   ├── scx_slo.bpf.c    # Kernel-side BPF scheduler logic
│   └── scx_slo.c         # Userspace control agent
├── include/
│   └── scx_slo.h         # Shared definitions between BPF and userspace
├── scripts/
│   └── (utility scripts)
├── test/
│   └── (test programs)
├── Makefile
└── README.md
```

## 2. Build System

Using a Makefile-based approach for simplicity:
- **BPF Compilation**: Use clang with BTF generation
- **Userspace Binary**: Link against libbpf
- **Skeleton Generation**: Use bpftool to generate BPF skeleton headers

## 3. Core Implementation Files

### src/scx_slo.bpf.c - BPF Scheduler Implementation
- Define `struct sched_ext_ops` with callbacks:
  - `enqueue`: Place tasks in deadline-ordered queue
  - `dispatch`: Select next task based on earliest deadline
  - `tick`: Update runtime statistics
- Implement deadline-based scheduling logic
- Create BPF maps:
  - `cfg_map`: cgroup_id → SLO configuration
  - `stats_map`: Runtime statistics per cgroup
  - Ring buffer for deadline miss events
- Virtual deadline calculation: `deadline = last_runtime + budget_ns`

### src/scx_slo.c - Userspace Agent
- Load and attach BPF program using libbpf
- Read SLO configurations (initially hardcoded)
- Update BPF maps with SLO parameters
- Monitor ring buffer for deadline misses
- Implement basic metrics exposure (stdout initially)
- Handle graceful shutdown and BPF program detachment

### include/scx_slo.h - Shared Definitions
```c
struct slo_cfg {
    u64 budget_ns;      // Latency budget in nanoseconds
    u32 importance;     // Relative importance weight
    u32 flags;          // Configuration flags
};

struct deadline_event {
    u64 cgroup_id;
    u64 deadline_miss_ns;
    u64 timestamp;
};
```

## 4. Dependencies & Prerequisites

### Required Packages
- `clang` (>= 16.0.0) - For BPF compilation
- `libbpf-dev` (>= 1.2.2) - BPF loading and management
- `linux-headers-$(uname -r)` - Kernel headers
- `bpftool` - BPF skeleton generation
- `pahole` (>= 1.25) - BTF generation

### Kernel Requirements
- Linux kernel 6.12+ with sched_ext support
- BTF enabled in kernel config

## 5. Build Steps

### Makefile Targets
1. **bpf**: Compile BPF program to .o with BTF
   ```bash
   clang -g -O2 -target bpf -D__TARGET_ARCH_x86_64 -c src/scx_slo.bpf.c -o src/scx_slo.bpf.o
   ```

2. **skel**: Generate skeleton header with bpftool
   ```bash
   bpftool gen skeleton src/scx_slo.bpf.o > src/scx_slo.skel.h
   ```

3. **userspace**: Compile userspace program
   ```bash
   gcc -g -O2 src/scx_slo.c -lbpf -lelf -lz -o scx_slo
   ```

4. **all**: Build everything
5. **clean**: Remove build artifacts
6. **install**: Copy binary to /usr/local/bin

## 6. Testing Infrastructure

### Test Programs
- `test/stress_test.sh`: Use stress-ng to generate CPU load
- `test/verify_scheduler.sh`: Check if scheduler is properly attached
- `test/deadline_test.c`: Program to test deadline enforcement

### Monitoring
- Script to verify scheduler attachment: `cat /sys/kernel/sched_ext/state`
- Performance metrics collection
- Deadline miss rate monitoring

## 7. Initial Implementation Milestones

### Phase 1: Minimal Working Scheduler
- Basic FIFO scheduling with sched_ext
- BPF program loads and attaches successfully
- Userspace agent starts and communicates with BPF

### Phase 2: SLO-Aware Scheduling
- Implement deadline-based scheduling
- Add cgroup→SLO mapping
- Basic deadline miss detection

### Phase 3: Production Features
- Ring buffer for deadline events
- Metrics exposure
- Graceful error handling
- Configuration file support

## 8. Development Notes

### Safety Considerations
- Kernel automatically falls back to CFS on BPF program crash
- Implement watchdog timeout handling
- Add comprehensive error logging

### Performance Optimization
- Minimize BPF map lookups in hot path
- Use per-CPU data structures where possible
- Batch map updates from userspace

### Future Enhancements
- Kubernetes integration (CRD/ConfigMap)
- Prometheus metrics endpoint
- Dynamic SLO adjustment based on load
- Multi-queue scheduling for better scalability